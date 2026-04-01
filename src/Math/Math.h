#pragma once

namespace dy::Math
{
	struct alignas(16) Vector3
	{
		float x, y, z;

		float w; // vector4 and satisfy alignment
		
		constexpr Vector3() : Vector3(0.0f,0.0f,0.0f) {}
		constexpr Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

		[[nodiscard]] Vector3 Normalize() const;
		[[nodiscard]] Vector3 Cross(const Vector3& rhs) const;
		[[nodiscard]] float Dot(const Vector3& rhs) const;

		Vector3 operator+(const Vector3& rhs) const;
		Vector3 operator-(const Vector3& rhs) const;
	};
	
	// Aligning the matrix to 64 bytes ensures it exactly fits one L1 cache line.
	// Prevents false sharing and cache line splitting across hardware threads.
	struct alignas(64) Matrix4x4
	{
		float m[16];
		
		[[nodiscard]] static Matrix4x4 Identity();
		[[nodiscard]] static Matrix4x4 CreateLookAtLH(const Vector3& eye, const Vector3& target, const Vector3& up);
		[[nodiscard]] static Matrix4x4 CreatePerspectiveFovLH(float fovY, float aspect, float nearZ, float farZ);

		[[nodiscard]] Matrix4x4 Multiply(const Matrix4x4& rhs) const;
		[[nodiscard]] Matrix4x4 MultiplySIMD(const Matrix4x4& rhs) const;
	};

	// The ultimate DOD batch processing function.
	// Operates on contiguous memory arrays for maximum cache locality and SIMD throughput.
	void MultiplyMatricesBatch(const Matrix4x4* lhs, const Matrix4x4* rhs, Matrix4x4* out, size_t count);
}

/* SIMD Libraries

하드웨어 종속성 : Intel, AMD(x86_64 arch)

Apple Silicon(M1/M2) 기반의 Mac이나 Android 기기(ARM arch)에서는 컴파일 에러가 터진다.
ARM은 NEON이라는 SIMD 인트린직(SIMD Intrinsic)을 사용한다

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

/* alignas(n)
*/

/*
[[nodiscard]]

반환 값을 받지 않거나 사용하지 않을 경우
인간의 실수

비용 : 런타임 오버헤드는 0 (컴파일 단계이기 때문)
*/

/* Flat 이란 무엇인가. Batch


*/