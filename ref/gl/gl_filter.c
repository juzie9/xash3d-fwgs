/*
gl_filter.c - real-time screen filter system (sharpen, exposure, contrast,
highlights, shadows, grayscale, hue, temperature, vibrance)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#include "gl_filter.h"

#if !XASH_GLES || !XASH_GL_STATIC

static qboolean	s_tried;
static GLuint	s_program;
static GLint	s_loc_tex;
static filter_locs_t s_locs;

static const char *filter_vs =
	"void main( void )\n"
	"{\n"
	"	gl_Position = ftransform();\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *filter_fs =
	"uniform sampler2D u_tex;\n"
	FILTER_GLSL_UNIFORMS
	FILTER_GLSL_MAIN
	"void main( void )\n"
	"{\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	vec3 color = texture2D( u_tex, uv ).rgb;\n"
	"	color = XashFilter( color, uv );\n"
	"	gl_FragColor = vec4( color, 1.0 );\n"
	"}\n";

static qboolean GL_FilterCompileShader( GLuint *out, GLenum type, const char *src )
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

static GLuint GL_FilterCreateProgram( const char *fs_src )
{
	GLuint vs = 0, fs = 0, prog = 0;
	GLint linked = 0;

	if( !GL_FilterCompileShader( &vs, GL_VERTEX_SHADER_ARB, filter_vs ))
		return 0;

	if( !GL_FilterCompileShader( &fs, GL_FRAGMENT_SHADER_ARB, fs_src ))
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
GL_FilterActive

tells whether the screen filter system is wanted, the actual
availability of render targets and GLSL is checked separately
================
*/
qboolean GL_FilterActive( void )
{
	return gl_filters.value != 0.0f
		&& GL_Support( GL_SHADER_OBJECTS_EXT )
		&& GL_Support( GL_SHADER_GLSL100_EXT );
}

/*
================
GL_FilterFreePrograms

releases the filter program, called when the GL context
is about to be torn down with the render targets
================
*/
void GL_FilterFreePrograms( void )
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
GL_FilterInitProgram

queries the filter uniform locations of an already created program,
shared between the blur composite and the standalone filter pass
================
*/
qboolean GL_FilterInitProgram( GLuint program, filter_locs_t *locs )
{
	locs->sharpen = pglGetUniformLocationARB( program, "u_flt_sharpen" );
	locs->exposure = pglGetUniformLocationARB( program, "u_flt_exposure" );
	locs->contrast = pglGetUniformLocationARB( program, "u_flt_contrast" );
	locs->highlights = pglGetUniformLocationARB( program, "u_flt_highlights" );
	locs->shadows = pglGetUniformLocationARB( program, "u_flt_shadows" );
	locs->grayscale = pglGetUniformLocationARB( program, "u_flt_grayscale" );
	locs->hue = pglGetUniformLocationARB( program, "u_flt_hue" );
	locs->hue_intensity = pglGetUniformLocationARB( program, "u_flt_hue_intensity" );
	locs->temperature = pglGetUniformLocationARB( program, "u_flt_temperature" );
	locs->vibrance = pglGetUniformLocationARB( program, "u_flt_vibrance" );
	locs->texel = pglGetUniformLocationARB( program, "u_flt_texel" );
	locs->sharpen_tex = pglGetUniformLocationARB( program, "u_flt_sharpen_tex" );

	return locs->sharpen != -1 && locs->exposure != -1 && locs->contrast != -1
		&& locs->highlights != -1 && locs->shadows != -1 && locs->grayscale != -1
		&& locs->hue != -1 && locs->hue_intensity != -1 && locs->temperature != -1
		&& locs->vibrance != -1 && locs->texel != -1 && locs->sharpen_tex != -1;
}

/*
================
GL_FilterSetUniforms

uploads the current CVar values, the texel size is taken from the
source texture so the sharpen kernel works at any resolution
================
*/
void GL_FilterSetUniforms( const filter_locs_t *locs, int sharpenTexUnit, const gl_rendertarget_t *screen )
{
	pglUniform1fARB( locs->sharpen, bound( 0.0f, gl_filters_sharpen.value, 2.0f ));
	pglUniform1fARB( locs->exposure, bound( 0.5f, gl_filters_exposure.value, 2.0f ));
	pglUniform1fARB( locs->contrast, bound( 0.5f, gl_filters_contrast.value, 1.5f ));
	pglUniform1fARB( locs->highlights, bound( -1.0f, gl_filters_highlights.value, 1.0f ));
	pglUniform1fARB( locs->shadows, bound( -1.0f, gl_filters_shadows.value, 1.0f ));
	pglUniform1fARB( locs->grayscale, bound( 0.0f, gl_filters_grayscale.value, 1.0f ));
	pglUniform1fARB( locs->hue, bound( -180.0f, gl_filters_hue.value, 180.0f ));
	pglUniform1fARB( locs->hue_intensity, bound( 0.0f, gl_filters_hue_intensity.value, 1.0f ));
	pglUniform1fARB( locs->temperature, bound( -1.0f, gl_filters_temperature.value, 1.0f ));
	pglUniform1fARB( locs->vibrance, bound( -1.0f, gl_filters_vibrance.value, 1.0f ));
	pglUniform2fARB( locs->texel, 1.0f / screen->width, 1.0f / screen->height );
	pglUniform1iARB( locs->sharpen_tex, sharpenTexUnit );
}

static void GL_FilterDrawQuad( const gl_rendertarget_t *dst )
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
GL_FilterScenePass

applies the filter chain to the rendered scene: reads the scene target
and writes the screen target before the HUD is drawn, so menus, the
crosshair and the console keep their original colors. Returns false
when the program can't be created, so callers can fall back to the
plain copy.
================
*/
qboolean GL_FilterScenePass( const gl_rendertarget_t *scene, const gl_rendertarget_t *screen )
{
	matrix4x4 m;

	// create the program once; every call after the init draws the pass
	if( !s_tried )
	{
		s_tried = true;

		if( !GL_Support( GL_SHADER_OBJECTS_EXT ) || !GL_Support( GL_SHADER_GLSL100_EXT ))
		{
			gEngfuncs.Con_Printf( S_WARN "gl_filters: GLSL shaders are not supported\n" );
			return false;
		}

		s_program = GL_FilterCreateProgram( filter_fs );

		if( !s_program || !GL_FilterInitProgram( s_program, &s_locs ))
		{
			gEngfuncs.Con_Printf( S_WARN "gl_filters: can't compile the filter shader\n" );
			GL_FilterFreePrograms();
			return false;
		}

		s_loc_tex = pglGetUniformLocationARB( s_program, "u_tex" );
	}

	if( !s_program )
		return false;

	pglBindFramebuffer( GL_FRAMEBUFFER, screen->fbo );

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

	GL_Bind( XASH_TEXTURE0, scene->texnum );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

	pglUseProgramObjectARB( s_program );
	pglUniform1iARB( s_loc_tex, 0 );
	GL_FilterSetUniforms( &s_locs, 0, scene );

	GL_FilterDrawQuad( screen );

	pglUseProgramObjectARB( 0 );
	pglTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

	return true;
}

#endif // !XASH_GLES || !XASH_GL_STATIC
