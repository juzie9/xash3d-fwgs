/*
gl_bloom.c - bloom post-processing

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

The bright parts of the scene are extracted with a soft threshold into
a half resolution target, blurred with the existing separable gaussian
and added back on top of the composed image by the composite shaders.
*/
#include "gl_bloom.h"

#if !XASH_GLES || !XASH_GL_STATIC

static qboolean	s_tried;
static GLuint	s_program;
static GLint	s_loc_tex;
static GLint	s_loc_threshold;

static const char *bloom_vs =
	"void main( void )\n"
	"{\n"
	"	gl_Position = ftransform();\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *bloom_fs =
	"uniform sampler2D u_tex;\n"
	"uniform float u_threshold;\n"
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	vec3 c = texture2D( u_tex, uv ).rgb;\n"
	"	float l = dot( c, vec3( 0.299, 0.587, 0.114 ));\n"
	"	// soft knee around the threshold keeps the transition smooth\n"
	"	vec3 bright = c * smoothstep( u_threshold - 0.15, u_threshold + 0.15, l );\n"
	"	gl_FragColor = vec4( bright, 1.0 );\n"
	"}\n";

static qboolean GL_BloomCompileShader( GLuint *out, GLenum type, const char *src )
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
GL_BloomActive

tells whether the bloom effect is wanted, the actual availability of
render targets and GLSL is checked separately
================
*/
qboolean GL_BloomActive( void )
{
	return gl_bloom.value != 0.0f
		&& GL_Support( GL_SHADER_OBJECTS_EXT )
		&& GL_Support( GL_SHADER_GLSL100_EXT );
}

/*
================
GL_BloomFreePrograms

releases the bloom bright pass program when the GL context is torn down
================
*/
void GL_BloomFreePrograms( void )
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
GL_BloomBrightPass

extracts the bright parts of the scene into the half resolution bloom
target, the program is created once
================
*/
qboolean GL_BloomBrightPass( const gl_rendertarget_t *dst, const gl_rendertarget_t *src )
{
	matrix4x4 m;

	if( !s_tried )
	{
		GLuint vs = 0, fs = 0;
		GLint linked = 0;

		s_tried = true;

		if( !GL_BloomCompileShader( &vs, GL_VERTEX_SHADER_ARB, bloom_vs ))
			return false;

		if( !GL_BloomCompileShader( &fs, GL_FRAGMENT_SHADER_ARB, bloom_fs ))
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
			GL_BloomFreePrograms();
			return false;
		}

		s_loc_tex = pglGetUniformLocationARB( s_program, "u_tex" );
		s_loc_threshold = pglGetUniformLocationARB( s_program, "u_threshold" );

		if( s_loc_tex == -1 || s_loc_threshold == -1 )
		{
			gEngfuncs.Con_Printf( S_WARN "gl_bloom: can't find the bloom shader uniforms\n" );
			GL_BloomFreePrograms();
			return false;
		}
	}

	if( !s_program || !dst->fbo || !src->texnum )
		return false;

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

	GL_Bind( XASH_TEXTURE0, src->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	pglUseProgramObjectARB( s_program );
	pglUniform1iARB( s_loc_tex, 0 );
	pglUniform1fARB( s_loc_threshold, bound( 0.0f, gl_bloom_threshold.value, 1.0f ));

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

	return true;
}

#endif // !XASH_GLES || !XASH_GL_STATIC
