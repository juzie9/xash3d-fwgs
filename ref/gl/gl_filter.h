/*
gl_filter.h - shared GLSL and interface for the screen filter system

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef GL_FILTER_H
#define GL_FILTER_H

#include "gl_local.h"

#if !XASH_GLES || !XASH_GL_STATIC

// single source of truth for the filter GLSL: the same chain runs both
// inside the blur composite and in the standalone scene filter pass
#define FILTER_GLSL_UNIFORMS \
	"uniform float u_flt_sharpen;\n" \
	"uniform float u_flt_exposure;\n" \
	"uniform float u_flt_contrast;\n" \
	"uniform float u_flt_highlights;\n" \
	"uniform float u_flt_shadows;\n" \
	"uniform float u_flt_grayscale;\n" \
	"uniform float u_flt_hue;\n" \
	"uniform float u_flt_hue_intensity;\n" \
	"uniform float u_flt_temperature;\n" \
	"uniform float u_flt_vibrance;\n" \
	"uniform vec2 u_flt_texel;\n" \
	"uniform sampler2D u_flt_sharpen_tex;\n"

#define FILTER_GLSL_MAIN \
	"vec3 XashFilter( vec3 color, vec2 uv )\n" \
	"{\n" \
	"	const vec3 luma_weights = vec3( 0.299, 0.587, 0.114 );\n" \
	"	float luma = dot( color, luma_weights );\n" \
	"	float mx = max( max( color.r, color.g ), color.b );\n" \
	"	float mn = min( min( color.r, color.g ), color.b );\n" \
	"	float sat = mx - mn;\n" \
	"	color *= u_flt_exposure;\n" \
	"	color = ( color - 0.5 ) * u_flt_contrast + 0.5;\n" \
	"	color += u_flt_highlights * smoothstep( 0.6, 1.0, luma );\n" \
	"	color += u_flt_shadows * ( 1.0 - smoothstep( 0.0, 0.45, luma ));\n" \
	"	color = mix( color, vec3( luma ), u_flt_grayscale );\n" \
	"	if( u_flt_hue_intensity > 0.0 )\n" \
	"	{\n" \
	"		const vec3 k = vec3( 0.57735 );\n" \
	"		float ang = radians( u_flt_hue ) * u_flt_hue_intensity;\n" \
	"		float cs = cos( ang );\n" \
	"		float sn = sin( ang );\n" \
	"		color = color * cs + cross( k, color ) * sn + k * dot( k, color ) * ( 1.0 - cs );\n" \
	"	}\n" \
	"	color.r *= 1.0 + u_flt_temperature * 0.15;\n" \
	"	color.b *= 1.0 - u_flt_temperature * 0.15;\n" \
	"	float newluma = dot( color, luma_weights );\n" \
	"	color *= luma / max( newluma, 0.001 );\n" \
	"	color = vec3( luma ) + ( color - vec3( luma )) * ( 1.0 + u_flt_vibrance * ( 1.0 - sat ));\n" \
	"	if( u_flt_sharpen > 0.0 )\n" \
	"	{\n" \
	"		vec3 c = texture2D( u_flt_sharpen_tex, uv ).rgb;\n" \
	"		vec3 u = texture2D( u_flt_sharpen_tex, uv + vec2( 0.0, u_flt_texel.y )).rgb;\n" \
	"		vec3 d = texture2D( u_flt_sharpen_tex, uv + vec2( 0.0, -u_flt_texel.y )).rgb;\n" \
	"		vec3 l = texture2D( u_flt_sharpen_tex, uv + vec2( -u_flt_texel.x, 0.0 )).rgb;\n" \
	"		vec3 r = texture2D( u_flt_sharpen_tex, uv + vec2( u_flt_texel.x, 0.0 )).rgb;\n" \
	"		color += u_flt_sharpen * ( 4.0 * c - u - d - l - r );\n" \
	"	}\n" \
	"	return color;\n" \
	"}\n"

typedef struct filter_locs_s
{
	GLint sharpen;
	GLint exposure;
	GLint contrast;
	GLint highlights;
	GLint shadows;
	GLint grayscale;
	GLint hue;
	GLint hue_intensity;
	GLint temperature;
	GLint vibrance;
	GLint texel;
	GLint sharpen_tex;
} filter_locs_t;

qboolean GL_FilterActive( void );
void GL_FilterFreePrograms( void );
qboolean GL_FilterInitProgram( GLuint program, filter_locs_t *locs );
void GL_FilterSetUniforms( const filter_locs_t *locs, int sharpenTexUnit, const gl_rendertarget_t *screen );
qboolean GL_FilterScenePass( const gl_rendertarget_t *scene, const gl_rendertarget_t *screen );

#endif // !XASH_GLES || !XASH_GL_STATIC

#endif // GL_FILTER_H
