#pragma once

namespace dy::Math
{
        struct Vector2 { float x, y; };
        struct Vector3 { float x, y, z; };
        struct Vector4 { float x, y, z, w; };
        struct Matrix4x4 { float m[4][4]; };
}
/* SIMD Libraries

<mmintrin.h>  MMX
<xmmintrin.h> SSE
<emmintrin.h> SSE2
<pmmintrin.h> SSE3
<tmmintrin.h> SSSE3
<smmintrin.h> SSE4.1
<nmmintrin.h> SSE4.2
<ammintrin.h> SSE4A
<wmmintrin.h> AES
<immintrin.h> AVX, AVX2, FMA
*/