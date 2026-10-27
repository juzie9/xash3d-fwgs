/*
gl_blur.c - gaussian blur post-process
Copyright (C) 2026 Alibek Omarov

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "gl_local.h"
#include "gl_filter.h"
#include "gl_bloom.h"

#if !XASH_GLES || !XASH_GL_STATIC

// 33 taps with 1-texel spacing: dense sampling, so even huge radii
// stay smooth without the ghosting of the fixed-function path
#define BLUR_TAPS 33

static GLuint	s_blur_program;
static GLuint	s_composite_program;
static GLint	s_blur_loc_tex;
static GLint	s_blur_loc_dir;
static GLint	s_blur_loc_weight;
static GLint	s_comp_loc_sharp;
static GLint	s_comp_loc_blur;
static GLint	s_comp_loc_depth;
static GLint	s_comp_loc_near;
static GLint	s_comp_loc_far;
static GLint	s_comp_loc_focus;
static GLint	s_comp_loc_range;
static GLint	s_comp_loc_darken;
static GLint	s_comp_loc_contrast;
static GLint	s_comp_loc_bloom_tex;
static GLint	s_comp_loc_bloom_intensity;
static filter_locs_t s_filter_locs;
static qboolean	s_shaders_tried;

static const char *blur_vs =
	"void main( void )\n"
	"{\n"
	"	gl_Position = ftransform();\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *blur_fs =
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_dir;\n"
	"uniform float u_weight[33];\n"
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	vec3 color = vec3( 0.0 );\n"
	"	float sum = 0.0;\n"
	"	int i;\n"
	"	for( i = 0; i < 33; i++ )\n"
	"	{\n"
	"		float off = float( i - 16 );\n"
	"		color += texture2D( u_tex, uv + u_dir * off ).rgb * u_weight[i];\n"
	"		sum += u_weight[i];\n"
	"	}\n"
	"	gl_FragColor = vec4( color / sum, 1.0 );\n"
	"}\n";

static const char *composite_fs =
	"uniform sampler2D u_sharp;\n"
	"uniform sampler2D u_blur;\n"
	"uniform sampler2D u_depth;\n"
	"uniform float u_near;\n"
	"uniform float u_far;\n"
	"uniform float u_focus;\n"
	"uniform float u_range;\n"
	"uniform float u_darken;\n"
	"uniform float u_contrast;\n"
	FILTER_GLSL_UNIFORMS
	BLOOM_GLSL_UNIFORMS
	FILTER_GLSL_MAIN
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	vec3 sharp = texture2D( u_sharp, uv ).rgb;\n"
	"	vec3 blurred = texture2D( u_blur, uv ).rgb;\n"
	"	float amount = 1.0;\n"
	"	if( u_focus > 0.0 )\n"
	"	{\n"
	"		float depth = texture2D( u_depth, uv ).r;\n"
	"		float zn = 2.0 * depth - 1.0;\n"
	"		float linear = 2.0 * u_near * u_far / ( u_far + u_near - zn * ( u_far - u_near ));\n"
	"		amount = smoothstep( u_focus, u_focus + u_range, linear );\n"
	"	}\n"
	"	vec3 color = mix( sharp, blurred, amount );\n"
	"	color *= 1.0 - u_darken * amount;\n"
	"	color = ( color - 0.5 ) * ( 1.0 + u_contrast ) + 0.5;\n"
	"	color = XashFilter( color, uv );\n"
	BLOOM_GLSL_ADD
	"	gl_FragColor = vec4( color, 1.0 );\n"
	"}\n";

static qboolean GL_BlurCompileShader( GLuint *out, GLenum type, const char *src )
{
	GLuint shader;
	GLint compiled = 0;

	shader = pglCreateShaderObjectARB( type );
	pglShaderSourceARB( shader, 1, (const char **)&src, NULL );
	pglCompileShaderARB( shader );
	pglGetObjectParameterivARB( shader, GL_OBJECT_COMPILE_STATUS_ARB, &compiled );

	if( !compiled )
	{
		char log[1024];
		GLsizei len = 0;

		pglGetInfoLogARB( shader, sizeof( log ), &len, log );
		gEngfuncs.Con_Printf( S_ERROR "%s: shader compile failed: %s\n", __func__, log );
		pglDeleteObjectARB( shader );
		return false;
	}

	*out = shader;
	return true;
}

static GLuint GL_BlurCreateProgram( const char *fs_src )
{
	GLuint vs = 0, fs = 0, prog = 0;
	GLint linked = 0;

	if( !GL_BlurCompileShader( &vs, GL_VERTEX_SHADER_ARB, blur_vs ))
		return 0;

	if( !GL_BlurCompileShader( &fs, GL_FRAGMENT_SHADER_ARB, fs_src ))
	{
		pglDeleteObjectARB( vs );
		return 0;
	}

	prog = pglCreateProgramObjectARB();
	pglAttachObjectARB( prog, vs );
	pglAttachObjectARB( prog, fs );
	pglLinkProgramARB( prog );
	pglGetObjectParameterivARB( prog, GL_OBJECT_LINK_STATUS_ARB, &linked );

	pglDeleteObjectARB( vs );
	pglDeleteObjectARB( fs );

	if( !linked )
	{
		char log[1024];
		GLsizei len = 0;

		pglGetInfoLogARB( prog, sizeof( log ), &len, log );
		gEngfuncs.Con_Printf( S_ERROR "%s: program link failed: %s\n", __func__, log );
		pglDeleteObjectARB( prog );
		return 0;
	}

	return prog;
}

/*
================
GL_BlurInitShaders

lazily compiles the blur and composite programs, falls back
to the fixed-function blur when GLSL is unavailable
================
*/
qboolean GL_BlurInitShaders( void )
{
	if( s_shaders_tried )
		return s_blur_program && s_composite_program;

	s_shaders_tried = true;

	if( !GL_Support( GL_SHADER_OBJECTS_EXT ) || !GL_Support( GL_SHADER_GLSL100_EXT ))
	{
		gEngfuncs.Con_Printf( S_WARN "gl_blur: GLSL shaders are not supported, using the fixed-function blur\n" );
		return false;
	}

	s_blur_program = GL_BlurCreateProgram( blur_fs );

	if( s_blur_program )
	{
		s_blur_loc_tex = pglGetUniformLocationARB( s_blur_program, "u_tex" );
		s_blur_loc_dir = pglGetUniformLocationARB( s_blur_program, "u_dir" );
		s_blur_loc_weight = pglGetUniformLocationARB( s_blur_program, "u_weight" );
	}

	s_composite_program = GL_BlurCreateProgram( composite_fs );

	if( s_composite_program )
	{
		s_comp_loc_sharp = pglGetUniformLocationARB( s_composite_program, "u_sharp" );
		s_comp_loc_blur = pglGetUniformLocationARB( s_composite_program, "u_blur" );
		s_comp_loc_depth = pglGetUniformLocationARB( s_composite_program, "u_depth" );
		s_comp_loc_near = pglGetUniformLocationARB( s_composite_program, "u_near" );
		s_comp_loc_far = pglGetUniformLocationARB( s_composite_program, "u_far" );
		s_comp_loc_focus = pglGetUniformLocationARB( s_composite_program, "u_focus" );
		s_comp_loc_range = pglGetUniformLocationARB( s_composite_program, "u_range" );
		s_comp_loc_darken = pglGetUniformLocationARB( s_composite_program, "u_darken" );
		s_comp_loc_contrast = pglGetUniformLocationARB( s_composite_program, "u_contrast" );
		s_comp_loc_bloom_tex = pglGetUniformLocationARB( s_composite_program, "u_bloom_tex" );
		s_comp_loc_bloom_intensity = pglGetUniformLocationARB( s_composite_program, "u_bloom_intensity" );
		GL_FilterInitProgram( s_composite_program, &s_filter_locs );
	}

	if( !s_blur_program || !s_composite_program )
	{
		GL_BlurFreePrograms();
		gEngfuncs.Con_Printf( S_WARN "gl_blur: can't compile blur shaders, using the fixed-function blur\n" );
		return false;
	}

	return true;
}

/*
================
GL_BlurFreePrograms

releases the GLSL programs, called when the GL context
is about to be torn down with the render targets
================
*/
void GL_BlurFreePrograms( void )
{
	if( s_blur_program )
	{
		pglDeleteObjectARB( s_blur_program );
		s_blur_program = 0;
	}

	if( s_composite_program )
	{
		pglDeleteObjectARB( s_composite_program );
		s_composite_program = 0;
	}

	s_shaders_tried = false;
}

/*
================
GL_BlurActive

tells whether the gaussian blur post-process is wanted,
the actual availability of render targets is checked separately
================
*/
qboolean GL_BlurActive( void )
{
	return bound( 0.0f, gl_blur.value, 2.0f ) >= 1.0f;
}

/*
================
GL_BlurDofActive

the depth of field needs the shader path and a depth texture
================
*/
qboolean GL_BlurDofActive( void )
{
	return gl_blur_focus.value > 0.0f && GL_Support( GL_DEPTH_TEXTURE )
		&& GL_Support( GL_SHADER_OBJECTS_EXT ) && GL_Support( GL_SHADER_GLSL100_EXT );
}

/*
================
GL_ViewmodelAfterBlur

gl_blur 1 blurs only the world scene: the viewmodel is drawn directly
into the screen target and the final composite pass keeps it sharp with
the help of the depth buffer, so the weapon stays in focus like through
a camera lens
================
*/
qboolean GL_ViewmodelAfterBlur( void )
{
	return GL_RenderTargetsActive( ) && bound( 0.0f, gl_blur.value, 2.0f ) >= 1.0f
		&& bound( 0.0f, gl_blur.value, 2.0f ) < 2.0f;
}

static void GL_BlurWeights( float radius, float *weights )
{
	// radius is in destination texels (half resolution),
	// the kernel keeps a 1-texel sampling density so large
	// radii can't produce ghosting
	const float sigma = Q_max( 0.5f, radius * 0.35f );
	float sum = 0.0f;
	int i;

	for( i = 0; i < BLUR_TAPS; i++ )
	{
		const float d = i - BLUR_TAPS / 2;

		weights[i] = expf( -0.5f * d * d / ( sigma * sigma ));
		sum += weights[i];
	}

	for( i = 0; i < BLUR_TAPS; i++ )
		weights[i] /= sum;
}

static void GL_BlurSetupQuad( const gl_rendertarget_t *dst )
{
	matrix4x4 m;

	pglBindFramebuffer( GL_FRAMEBUFFER, dst->fbo );

	pglViewport( 0, 0, dst->width, dst->height );
	Matrix4x4_CreateOrtho( m, 0, dst->width, dst->height, 0, -99999, 99999 );
	pglMatrixMode( GL_PROJECTION );
	GL_LoadMatrix( m );

	Matrix4x4_LoadIdentity( m );
	pglMatrixMode( GL_MODELVIEW );
	GL_LoadMatrix( m );

	GL_Cull( GL_NONE );
	pglDepthMask( GL_FALSE );
	pglDisable( GL_DEPTH_TEST );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_FOG );
	pglDisable( GL_BLEND );
}

static void GL_BlurDrawQuad( const gl_rendertarget_t *dst )
{
	pglBegin( GL_QUADS );
		pglTexCoord2f( 0.0f, 1.0f );
		pglVertex2f( 0.0f, 0.0f );
		pglTexCoord2f( 1.0f, 1.0f );
		pglVertex2f( dst->width, 0.0f );
		pglTexCoord2f( 1.0f, 0.0f );
		pglVertex2f( dst->width, dst->height );
		pglTexCoord2f( 0.0f, 0.0f );
		pglVertex2f( 0.0f, dst->height );
	pglEnd();
}

/*
================
GL_BlurShaderPass

separable gaussian blur in a fragment shader: samples are taken
at every texel, so the result stays smooth at any radius
================
*/
void GL_BlurShaderPass( const gl_rendertarget_t *dst, const gl_rendertarget_t *src, float dirx, float diry, float radius )
{
	float weights[BLUR_TAPS];

	if( !dst->fbo || !src->texnum )
		return;

	GL_BlurSetupQuad( dst );

	GL_Bind( XASH_TEXTURE0, src->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	GL_BlurWeights( radius, weights );

	pglUseProgramObjectARB( s_blur_program );
	pglUniform1iARB( s_blur_loc_tex, 0 );
	pglUniform2fARB( s_blur_loc_dir, dirx / dst->width, diry / dst->height );
	pglUniform1fvARB( s_blur_loc_weight, BLUR_TAPS, weights );

	GL_BlurDrawQuad( dst );

	pglUseProgramObjectARB( 0 );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
}

/*
================
GL_BlurCompositePass

combines the sharp scene, the blurred scene and the depth texture
into the screen target: in gl_blur 1 the depth buffer keeps the
already drawn viewmodel sharp, the depth of field smoothly ramps
the defocus beyond gl_blur_focus
================
*/
static void GL_BlurCompositePass( const gl_rendertarget_t *scene, const gl_rendertarget_t *blurred, const gl_rendertarget_t *screen )
{
	float nearclip, farclip;

	if( !screen->fbo || !scene->texnum || !blurred->texnum )
		return;

	GL_BlurSetupQuad( screen );

	if( GL_ViewmodelAfterBlur( ))
	{
		// the quad sits at depth 0.5: the cleared background is 1.0
		// and the viewmodel drew its depth below 0.3, so only the
		// background gets composited and the weapon stays sharp
		pglEnable( GL_DEPTH_TEST );
		pglDepthFunc( GL_LEQUAL );
	}

	GL_Bind( XASH_TEXTURE0, scene->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	GL_Bind( XASH_TEXTURE1, blurred->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	if( GL_BlurDofActive( ) && scene->depthtex )
	{
		// depthtex is a raw GL texture name, not an engine texture index:
		// bind it directly, the bookkeeping is restored below
		GL_SelectTexture( XASH_TEXTURE2 );
		pglBindTexture( GL_TEXTURE_2D, scene->depthtex );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	}
	else
	{
		GL_Bind( XASH_TEXTURE2, tr.defaultTexture );
	}

	if( GL_BloomActive( ) && tr.targets.bloom.texnum )
		GL_Bind( XASH_TEXTURE3, tr.targets.bloom.texnum );
	else
		GL_Bind( XASH_TEXTURE3, tr.defaultTexture );

	// linearize depth with the projection of the main view:
	// row-major matrix, so use [row][column] indexing
	nearclip = RI.projectionMatrix[2][3] / ( RI.projectionMatrix[2][2] - 1.0f );
	farclip = RI.projectionMatrix[2][3] / ( RI.projectionMatrix[2][2] + 1.0f );

	if( nearclip <= 0.0f || farclip <= 0.0f || nearclip >= farclip )
	{
		nearclip = 4.0f;
		farclip = 4096.0f;
	}

	pglUseProgramObjectARB( s_composite_program );
	pglUniform1iARB( s_comp_loc_sharp, 0 );
	pglUniform1iARB( s_comp_loc_blur, 1 );
	pglUniform1iARB( s_comp_loc_depth, 2 );
	pglUniform1iARB( s_comp_loc_bloom_tex, 3 );
	pglUniform1fARB( s_comp_loc_bloom_intensity, GL_BloomActive( ) ? bound( 0.0f, gl_bloom_intensity.value, 2.0f ) : 0.0f );
	pglUniform1fARB( s_comp_loc_near, nearclip );
	pglUniform1fARB( s_comp_loc_far, farclip );
	pglUniform1fARB( s_comp_loc_focus, gl_blur_focus.value );
	pglUniform1fARB( s_comp_loc_range, Q_max( 1.0f, gl_blur_focus_range.value ));
	pglUniform1fARB( s_comp_loc_darken, bound( 0.0f, gl_blur_darken.value, 1.0f ));
	pglUniform1fARB( s_comp_loc_contrast, bound( -0.5f, gl_blur_contrast.value, 1.0f ));
	GL_FilterSetUniforms( &s_filter_locs, 0, 3, scene );

	GL_BlurDrawQuad( screen );

	pglUseProgramObjectARB( 0 );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	pglDisable( GL_DEPTH_TEST );

	// the composite enabled texture units 1 and 2: disable them and fix
	// the bookkeeping, otherwise the next 3D pass would modulate every
	// surface with the leftover blur texture and the picture goes dark
	GL_CleanUpTextureUnits( 1 );
}

/*
================
GL_BlurPass

legacy fixed-function fallback for backends without GLSL:
separable gaussian with additive blending. It keeps a limited
kernel support, so very large radii may show ghosting, the
shader path above is preferred when available
================
*/
static void GL_BlurPass( const gl_rendertarget_t *dst, const gl_rendertarget_t *src, int dirx, int diry, qboolean preserveViewmodel )
{
	// 13-tap binomial approximation of the gaussian curve
	static const float blur_weights[13] =
	{
		1.0f / 4096.0f, 12.0f / 4096.0f, 66.0f / 4096.0f, 220.0f / 4096.0f, 495.0f / 4096.0f, 792.0f / 4096.0f,
		924.0f / 4096.0f, 792.0f / 4096.0f, 495.0f / 4096.0f, 220.0f / 4096.0f, 66.0f / 4096.0f, 12.0f / 4096.0f, 1.0f / 4096.0f,
	};
	const float radius = Q_max( 0.1f, gl_blur_radius.value );
	matrix4x4 m;
	int i;

	if( !dst->fbo || !src->texnum )
	{
		gEngfuncs.Con_Printf( S_ERROR "%s: invalid render targets\n", __func__ );
		return;
	}

	pglBindFramebuffer( GL_FRAMEBUFFER, dst->fbo );

	pglViewport( 0, 0, dst->width, dst->height );
	Matrix4x4_CreateOrtho( m, 0, dst->width, dst->height, 0, -99999, 99999 );
	pglMatrixMode( GL_PROJECTION );
	GL_LoadMatrix( m );

	Matrix4x4_LoadIdentity( m );
	pglMatrixMode( GL_MODELVIEW );
	GL_LoadMatrix( m );

	GL_Cull( GL_NONE );

	if( preserveViewmodel )
	{
		pglEnable( GL_DEPTH_TEST );
		pglDepthFunc( GL_LEQUAL );
	}
	else
	{
		pglDisable( GL_DEPTH_TEST );
	}

	pglDepthMask( GL_FALSE );
	pglDisable( GL_ALPHA_TEST );
	pglDisable( GL_FOG );

	GL_Bind( XASH_TEXTURE0, src->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE ); // tap weight comes from the primary color

	for( i = 0; i < 13; i++ )
	{
		const float offset = ( i - 6 ) * radius;
		const float du = offset * dirx / dst->width;
		const float dv = offset * diry / dst->height;
		const float weight = blur_weights[i];

		if( i == 0 )
			pglDisable( GL_BLEND ); // first tap replaces the target contents
		else
		{
			pglEnable( GL_BLEND );
			pglBlendFunc( GL_ONE, GL_ONE );
		}

		pglColor4f( weight, weight, weight, weight );

		pglBegin( GL_QUADS );
			pglTexCoord2f( 0.0f + du, 1.0f + dv );
			pglVertex2f( 0.0f, 0.0f );
			pglTexCoord2f( 1.0f + du, 1.0f + dv );
			pglVertex2f( dst->width, 0.0f );
			pglTexCoord2f( 1.0f + du, 0.0f + dv );
			pglVertex2f( dst->width, dst->height );
			pglTexCoord2f( 0.0f + du, 0.0f + dv );
			pglVertex2f( 0.0f, dst->height );
		pglEnd();
	}

	// restore render state for the 2D drawing that follows
	pglDisable( GL_BLEND );
	pglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	pglDisable( GL_DEPTH_TEST );
	pglColor4f( 1.0f, 1.0f, 1.0f, 1.0f ); // the taps left the primary color at the last weight
}

/*
================
GL_BlurSceneTarget

two-pass separable gaussian blur of the rendered scene:
scene -> blur (horizontal, downsampled) -> blur2 (vertical) ->
screen (composite with depth of field). The shader path is used
when GLSL is available, otherwise the legacy fixed-function
multi-pass path is kept as a fallback.
================
*/
void GL_BlurSceneTarget( const gl_rendertarget_t *scene, const gl_rendertarget_t *blur, const gl_rendertarget_t *blur2, const gl_rendertarget_t *screen )
{
	if( GL_BlurInitShaders( ))
	{
		const float radius = Q_max( 0.1f, gl_blur_radius.value ) * 0.5f;

		GL_BlurShaderPass( blur, scene, 1.0f, 0.0f, radius ); // horizontal, also downsamples
		GL_BlurShaderPass( blur2, blur, 0.0f, 1.0f, radius ); // vertical
		GL_BlurCompositePass( scene, blur2, screen );
	}
	else
	{
		GL_BlurPass( blur, scene, 1, 0, false );
		GL_BlurPass( screen, blur, 0, 1, GL_ViewmodelAfterBlur( ));
	}
}

/*
================
GL_BindViewmodelTarget

binds the screen target for the viewmodel (gl_blur 1):
clears its color and depth and maps the viewport to the window,
so the weapon is drawn directly on top of where the blurred
background will appear
================
*/
void GL_BindViewmodelTarget( void )
{
	int x, x2, y, y2;

	if( !GL_RenderTargetsActive( ))
		return;

	pglBindFramebuffer( GL_FRAMEBUFFER, tr.targets.screen.fbo );

	// the screen target is window-sized: map the viewport like R_SetupGL does
	x = RI.rvp.viewport[0];
	x2 = RI.rvp.viewport[0] + RI.rvp.viewport[2];
	y = gpGlobals->height - RI.rvp.viewport[1];
	y2 = gpGlobals->height - ( RI.rvp.viewport[1] + RI.rvp.viewport[3] );

	if( tr.rotation & 1 )
		pglViewport( y2, x, y - y2, x2 - x );
	else
		pglViewport( x, y2, x2 - x, y - y2 );

	// start with a clean slate: the composite fills the background afterwards
	pglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	pglClearDepth( gldepthmax );
	pglClear( GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT );

	pglEnable( GL_DEPTH_TEST );
	pglDepthMask( GL_TRUE );
	pglDepthFunc( GL_LEQUAL );
}

#endif // !XASH_GLES || !XASH_GL_STATIC
