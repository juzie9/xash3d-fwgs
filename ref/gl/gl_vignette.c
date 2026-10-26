/*
gl_vignette.c - vignette (corner darkening) post-processing effect

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

#if !XASH_GLES || !XASH_GL_STATIC

static qboolean	s_tried;
static GLuint	s_program;
static GLint	s_loc_tex;
static GLint	s_loc_aspect;
static GLint	s_loc_strength;
static GLint	s_loc_radius;
static GLint	s_loc_softness;

static const char *vignette_vs =
	"void main( void )\n"
	"{\n"
	"	gl_Position = ftransform();\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *vignette_fs =
	"uniform sampler2D u_tex;\n"
	"uniform float u_aspect;\n"
	"uniform float u_strength;\n"
	"uniform float u_radius;\n"
	"uniform float u_softness;\n"
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	vec2 p = uv - vec2( 0.5 );\n"
	/* keep the vignette circular regardless of the aspect ratio */
	"	p.x *= u_aspect;\n"
	"	float d = length( p );\n"
	"	float v = u_strength * smoothstep( u_radius - u_softness, u_radius + u_softness, d );\n"
	"	vec3 color = texture2D( u_tex, uv ).rgb * ( 1.0 - v );\n"
	"	float h = fract( sin( dot( gl_FragCoord.xy, vec2( 12.9898, 78.233 ))) * 43758.5453 );\n"
	/* 1-bit dither to hide banding in the gradient */
	"	color += ( h - 0.5 ) * ( 2.0 / 255.0 );\n"
	"	gl_FragColor = vec4( color, 1.0 );\n"
	"}\n";

static qboolean GL_VignetteCompileShader( GLuint *out, GLenum type, const char *src )
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

static GLuint GL_VignetteCreateProgram( const char *fs_src )
{
	GLuint vs = 0, fs = 0, prog = 0;
	GLint linked = 0;

	if( !GL_VignetteCompileShader( &vs, GL_VERTEX_SHADER_ARB, vignette_vs ))
		return 0;

	if( !GL_VignetteCompileShader( &fs, GL_FRAGMENT_SHADER_ARB, fs_src ))
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
GL_VignetteInitShaders

lazily compiles the vignette program
================
*/
static qboolean GL_VignetteInitShaders( void )
{
	if( s_tried )
		return s_program != 0;

	s_tried = true;

	if( !GL_Support( GL_SHADER_OBJECTS_EXT ) || !GL_Support( GL_SHADER_GLSL100_EXT ))
	{
		gEngfuncs.Con_Printf( S_WARN "gl_vignette: GLSL shaders are not supported\n" );
		return false;
	}

	s_program = GL_VignetteCreateProgram( vignette_fs );

	if( !s_program )
	{
		gEngfuncs.Con_Printf( S_WARN "gl_vignette: can't compile the vignette shader\n" );
		return false;
	}

	s_loc_tex = pglGetUniformLocationARB( s_program, "u_tex" );
	s_loc_aspect = pglGetUniformLocationARB( s_program, "u_aspect" );
	s_loc_strength = pglGetUniformLocationARB( s_program, "u_strength" );
	s_loc_radius = pglGetUniformLocationARB( s_program, "u_radius" );
	s_loc_softness = pglGetUniformLocationARB( s_program, "u_softness" );

	return true;
}

/*
================
GL_VignetteFreePrograms

releases the vignette program, called when the GL context
is about to be torn down with the render targets
================
*/
void GL_VignetteFreePrograms( void )
{
	if( s_program )
	{
		pglDeleteObjectARB( s_program );
		s_program = 0;
	}

	s_tried = false;
}

/*
================
GL_VignetteActive

tells whether the vignette post-process is wanted, the actual
availability of render targets and GLSL is checked separately
================
*/
qboolean GL_VignetteActive( void )
{
	return gl_vignette.value != 0.0f
		&& GL_Support( GL_SHADER_OBJECTS_EXT )
		&& GL_Support( GL_SHADER_GLSL100_EXT );
}

static void GL_VignetteDrawQuad( const gl_rendertarget_t *dst )
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
GL_VignetteDraw

draws the screen target into the window with the vignette applied:
runs after the blur composite (or the plain scene copy), so the
vignette always affects the final composed image exactly once
================
*/
qboolean GL_VignetteDraw( const gl_rendertarget_t *screen )
{
	matrix4x4 m;

	if( !GL_VignetteInitShaders( ))
		return false;

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	pglViewport( 0, 0, screen->width, screen->height );
	Matrix4x4_CreateOrtho( m, 0, screen->width, screen->height, 0, -99999, 99999 );
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

	GL_Bind( XASH_TEXTURE0, screen->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	pglUseProgramObjectARB( s_program );
	pglUniform1iARB( s_loc_tex, 0 );
	pglUniform1fARB( s_loc_aspect, ( float )screen->width / ( float )screen->height );
	pglUniform1fARB( s_loc_strength, bound( 0.0f, gl_vignette_strength.value, 1.0f ));
	pglUniform1fARB( s_loc_radius, Q_max( 0.0f, gl_vignette_radius.value ));
	pglUniform1fARB( s_loc_softness, Q_max( 0.01f, gl_vignette_softness.value ));

	GL_VignetteDrawQuad( screen );

	pglUseProgramObjectARB( 0 );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	return true;
}

#endif // !XASH_GLES || !XASH_GL_STATIC
