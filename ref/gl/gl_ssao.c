/*
gl_ssao.c - screen space ambient occlusion

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

The occlusion factor is computed from the scene depth at half
resolution with a rotating kernel, blurred with the existing separable
gaussian and multiplied into the composed scene.
*/
#include "gl_ssao.h"

#if !XASH_GLES || !XASH_GL_STATIC

#define SSAO_NOISE_SIZE 8

static qboolean	s_tried;
static GLuint	s_program;
static GLint	s_loc_depth;
static GLint	s_loc_noise;
static GLint	s_loc_texel;
static GLint	s_loc_radius;
static GLint	s_loc_bias;
static GLint	s_loc_noise_scale;
static GLuint	s_noise_texnum;

static const char *ssao_vs =
	"void main( void )\n"
	"{\n"
	"	gl_Position = ftransform();\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *ssao_fs =
	"uniform sampler2D u_depth;\n"
	"uniform sampler2D u_noise;\n"
	"uniform vec2 u_texel;\n"
	"uniform float u_radius;\n"
	"uniform float u_bias;\n"
	"uniform vec2 u_noise_scale;\n"
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	float center = texture2D( u_depth, uv ).r;\n"
	"	vec2 rnd = texture2D( u_noise, uv * u_noise_scale ).rg * 2.0 - 1.0;\n"
	"	float ang = rnd.x * 6.28318;\n"
	"	float cs = cos( ang );\n"
	"	float sn = sin( ang );\n"
	"	float occ = 0.0;\n"
	"	vec2 dir;\n"
	"	vec2 tap;\n"
	"	dir = vec2( 1.0, 0.0 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	dir = vec2( 0.5, 0.866 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	dir = vec2( -0.5, 0.866 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	dir = vec2( -1.0, 0.0 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	dir = vec2( -0.5, -0.866 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	dir = vec2( 0.5, -0.866 ); tap = vec2( dir.x * cs - dir.y * sn, dir.x * sn + dir.y * cs ) * u_radius * u_texel;\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.33 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap * 0.66 ).r );\n"
	"	occ += step( u_bias, center - texture2D( u_depth, uv + tap ).r );\n"
	"	occ /= 18.0;\n"
	"	gl_FragColor = vec4( occ, 0.0, 0.0, 1.0 );\n"
	"}\n";

static void GL_SsaoCreateNoiseTexture( void )
{
	static byte noise[SSAO_NOISE_SIZE * SSAO_NOISE_SIZE * 4];
	static qboolean inited = false;

	// deterministic pseudo random directions, tiled across the screen
	if( !inited )
	{
		inited = true;

		for( int i = 0; i < SSAO_NOISE_SIZE * SSAO_NOISE_SIZE; i++ )
		{
			const float ang = ( float )i * 2.399963f; // golden angle

			noise[i*4+0] = ( byte )(( cosf( ang ) * 0.5f + 0.5f ) * 255.0f );
			noise[i*4+1] = ( byte )(( sinf( ang ) * 0.5f + 0.5f ) * 255.0f );
			noise[i*4+2] = 0;
			noise[i*4+3] = 255;
		}
	}

	pglGenTextures( 1, &s_noise_texnum );
	pglBindTexture( GL_TEXTURE_2D, s_noise_texnum );
	glState.currentTextures[glState.activeTMU] = 0;
	glState.currentTexturesIndex[glState.activeTMU] = 0;

	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );

	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, SSAO_NOISE_SIZE, SSAO_NOISE_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, noise );
}

static qboolean GL_SsaoCompileShader( GLuint *out, GLenum type, const char *src )
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

/*
================
GL_SsaoActive

tells whether the SSAO effect is wanted, the actual availability of
render targets and GLSL is checked separately
================
*/
qboolean GL_SsaoActive( void )
{
	return gl_ssao.value != 0.0f
		&& GL_Support( GL_SHADER_OBJECTS_EXT )
		&& GL_Support( GL_SHADER_GLSL100_EXT );
}

/*
================
GL_SsaoFreePrograms

releases the SSAO program and the noise texture when the GL context
is torn down
================
*/
void GL_SsaoFreePrograms( void )
{
	if( s_program )
	{
		pglDeleteObjectARB( s_program );
		s_program = 0;
	}

	if( s_noise_texnum )
	{
		pglDeleteTextures( 1, &s_noise_texnum );
		s_noise_texnum = 0;
	}

	s_tried = false;
}

/*
================
GL_SsaoPass

computes the occlusion factor from the scene depth into the half
resolution ssao target, the program is created once
================
*/
qboolean GL_SsaoPass( const gl_rendertarget_t *dst, const gl_rendertarget_t *scene )
{
	matrix4x4 m;

	if( !s_tried )
	{
		GLuint vs = 0, fs = 0;
		GLint linked = 0;

		s_tried = true;

		if( !GL_SsaoCompileShader( &vs, GL_VERTEX_SHADER_ARB, ssao_vs ))
			return false;

		if( !GL_SsaoCompileShader( &fs, GL_FRAGMENT_SHADER_ARB, ssao_fs ))
		{
			pglDeleteObjectARB( vs );
			return false;
		}

		s_program = pglCreateProgramObjectARB();
		pglAttachObjectARB( s_program, vs );
		pglAttachObjectARB( s_program, fs );
		pglLinkProgramARB( s_program );
		pglGetObjectParameterivARB( s_program, GL_OBJECT_LINK_STATUS_ARB, &linked );

		pglDeleteObjectARB( vs );
		pglDeleteObjectARB( fs );

		if( !linked )
		{
			char log[1024];
			GLsizei len = 0;

			pglGetInfoLogARB( s_program, sizeof( log ), &len, log );
			gEngfuncs.Con_Printf( S_ERROR "%s: program link failed: %s\n", __func__, log );
			GL_SsaoFreePrograms();
			return false;
		}

		s_loc_depth = pglGetUniformLocationARB( s_program, "u_depth" );
		s_loc_noise = pglGetUniformLocationARB( s_program, "u_noise" );
		s_loc_texel = pglGetUniformLocationARB( s_program, "u_texel" );
		s_loc_radius = pglGetUniformLocationARB( s_program, "u_radius" );
		s_loc_bias = pglGetUniformLocationARB( s_program, "u_bias" );
		s_loc_noise_scale = pglGetUniformLocationARB( s_program, "u_noise_scale" );

		if( s_loc_depth == -1 || s_loc_noise == -1 || s_loc_texel == -1
			|| s_loc_radius == -1 || s_loc_bias == -1 || s_loc_noise_scale == -1 )
		{
			gEngfuncs.Con_Printf( S_WARN "gl_ssao: can't find the SSAO shader uniforms\n" );
			GL_SsaoFreePrograms();
			return false;
		}
	}

	if( !s_program || !dst->fbo || !scene->depthtex )
		return false;

	if( !s_noise_texnum )
		GL_SsaoCreateNoiseTexture();

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

	// the scene depth is a raw GL texture name, bind it directly
	GL_SelectTexture( XASH_TEXTURE0 );
	pglBindTexture( GL_TEXTURE_2D, scene->depthtex );
	glState.currentTextures[0] = 0;
	glState.currentTexturesIndex[0] = 0;

	// the noise texture is raw too
	GL_SelectTexture( XASH_TEXTURE1 );
	pglBindTexture( GL_TEXTURE_2D, s_noise_texnum );
	glState.currentTextures[1] = 0;
	glState.currentTexturesIndex[1] = 0;

	pglUseProgramObjectARB( s_program );
	pglUniform1iARB( s_loc_depth, 0 );
	pglUniform1iARB( s_loc_noise, 1 );
	pglUniform2fARB( s_loc_texel, 1.0f / dst->width, 1.0f / dst->height );
	pglUniform1fARB( s_loc_radius, bound( 1.0f, gl_ssao_radius.value, 32.0f ));
	pglUniform1fARB( s_loc_bias, bound( 0.0f, gl_ssao_bias.value, 0.1f ));
	pglUniform2fARB( s_loc_noise_scale, ( float )dst->width / SSAO_NOISE_SIZE, ( float )dst->height / SSAO_NOISE_SIZE );

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

	pglUseProgramObjectARB( 0 );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	GL_CleanUpTextureUnits( 1 );

	return true;
}

#endif // !XASH_GLES || !XASH_GL_STATIC
