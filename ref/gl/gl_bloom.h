/*
gl_bloom.h - shared GLSL for the bloom composite

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/
#ifndef GL_BLOOM_H
#define GL_BLOOM_H

#include "gl_local.h"

#if !XASH_GLES || !XASH_GL_STATIC

// the bloom add is folded into the existing composite shaders: when the
// bloom is disabled the intensity is zero and the add is an identity
#define BLOOM_GLSL_UNIFORMS \
	"uniform sampler2D u_bloom_tex;\n" \
	"uniform float u_bloom_intensity;\n"

#define BLOOM_GLSL_ADD \
	"	color += texture2D( u_bloom_tex, uv ).rgb * u_bloom_intensity;\n"

#endif // !XASH_GLES || !XASH_GL_STATIC

#endif // GL_BLOOM_H
