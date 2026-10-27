/*
gl_ssao.h - shared GLSL for the SSAO composite

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef GL_SSAO_H
#define GL_SSAO_H

#include "gl_local.h"

#if !XASH_GLES || !XASH_GL_STATIC

// the ambient occlusion is multiplied into the composed scene: when the
// effect is disabled the strength is zero and the multiply is an identity
#define SSAO_GLSL_UNIFORMS \
	"uniform sampler2D u_ssao_tex;\n" \
	"uniform float u_ssao_strength;\n"

#define SSAO_GLSL_ADD \
	"	color *= 1.0 - texture2D( u_ssao_tex, uv ).r * u_ssao_strength;\n"

#endif // !XASH_GLES || !XASH_GL_STATIC

#endif // GL_SSAO_H
