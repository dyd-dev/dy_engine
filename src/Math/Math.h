#pragma once
#include <cstddef> // size_t
/* Math.h
	강제 인라인(Force Inline)을 통한 함수 호출 오버헤드 제거를 위해 Header-Only를 유지한다.
	.cpp로 숨길 경우 LTO(Link Time Optimization)에 의존해야 하며 병목이 심해지며 인라이닝이 실패할 확룔이 높아진다. // .cpp가 없다면 링킹할 일도 없으므로
*/

/* SIMD
	과거 Intel 기반 Mac에서는 정상적으로 작동했으나
	현재 Apple Silicon(M1,M2,M3...)은 ARM64 아키텍처를 사용한다.
	ARM64 환경에서는 NEON이라는 자체 SIMD 명령어 셋을 사용하며 <arm_neon.h> 헤더를 사용한다.

[[nodiscard]]
	반환 값을 받지 않거나 사용하지 않을 경우. 인간의 실수 방지
	비용 : 런타임 오버헤드는 0 (컴파일 단계이기 때문)
*/

#if defined(__x86_64__) || defined(_M_X64)
	#define DY_SIMD_X64
	#include <immintrin.h> // SSE/AVX
#elif defined(__aarch64__) || defined(_M_ARM64)
	#define DY_SIMD_ARM64
	#include <arm_neon.h> // ARM NEON
#else
	#error "dy_engine: Unsupported hardware architecture for SIMD."
#endif

namespace dy::Math
{
	struct alignas(16) float3
	{
		union {
			struct { float x, y, z, pad; };
			float data[4];
#if defined(DY_SIMD_X64)
			__m128 simd;
#elif defined(DY_SIMD_ARM64)
			float32x4_t simd;			
#endif
		};

		inline float3() = default;
		inline float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z), pad(0.0f) {}
	};
	struct alignas(16) float4
	{
		union
		{
			struct { float x, y, z, w; };
			float data[4];
#if defined(DY_SIMD_X64)
			__m128 simd;
#elif defined(DY_SIMD_ARM64)
			float32x4_t simd;			
#endif
		};

		inline float4() = default;
		inline float4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
	};

	struct alignas(16) float4x4
	{
		float m[16];
		
		inline float4x4() = default;

		[[nodiscard]] static inline float4x4 Identity()
		{
			float4x4 res;
            res.m[0]=1.0f; res.m[1]=0.0f; res.m[2]=0.0f; res.m[3]=0.0f;
            res.m[4]=0.0f; res.m[5]=1.0f; res.m[6]=0.0f; res.m[7]=0.0f;
            res.m[8]=0.0f; res.m[9]=0.0f; res.m[10]=1.0f; res.m[11]=0.0f;
            res.m[12]=0.0f; res.m[13]=0.0f; res.m[14]=0.0f; res.m[15]=1.0f;
			return res;
		}
		
		[[nodiscard]] inline float4x4 Multiply(const float4x4& rhs) const
		{
			float4x4 res;
#if defined(DY_SIMD_X64)
			for(int i=0; i<4; i++)
			{
				__m128 row = _mm_load_ps(&m[i * 4]);
				
				__m128 x = _mm_shuffle_ps(row, row, _MM_SHUFFLE(0, 0, 0, 0));
				__m128 y = _mm_shuffle_ps(row, row, _MM_SHUFFLE(1, 1, 1, 1));
				__m128 z = _mm_shuffle_ps(row, row, _MM_SHUFFLE(2, 2, 2, 2));
				__m128 w = _mm_shuffle_ps(row, row, _MM_SHUFFLE(3, 3, 3, 3));

				__m128 r0 = _mm_load_ps(&rhs.m[0]);
				__m128 r1 = _mm_load_ps(&rhs.m[4]);
				__m128 r2 = _mm_load_ps(&rhs.m[8]);
				__m128 r3 = _mm_load_ps(&rhs.m[12]);

				__m128 out = _mm_add_ps(
				    _mm_add_ps(_mm_mul_ps(x, r0), _mm_mul_ps(y, r1)),
				    _mm_add_ps(_mm_mul_ps(z, r2), _mm_mul_ps(w, r3))
				);
				_mm_store_ps(&res.m[i * 4], out);
			}
#elif defined(DY_SIMD_ARM64)
			for(int i=0; i<4; i++)
			{
				float32x4_t row = vld1q_f32(&m[i*4]);
				
				float32x4_t out = vmulq_laneq_f32(vld1q_f32(&rhs.m[0]), row, 0);
				out = vfmaq_laneq_f32(out, vld1q_f32(&rhs.m[4]), row, 1);
				out = vfmaq_laneq_f32(out, vld1q_f32(&rhs.m[8]), row, 2);
				out = vfmaq_laneq_f32(out, vld1q_f32(&rhs.m[12]), row, 3);

				vst1q_f32(&res.m[i*4], out);
			}
#endif
			return res;
		}
	};

	inline void MultiplyMatricesBatch(const float4x4* lhs, const float4x4* rhs, float4x4* out, size_t count)
	{
		// DOD approach: iterate linearly through aligned memory.
		// Hardware prefetchers will aggressively cache these arrays.
		for(size_t i = 0; i < count; ++i)
			out[i] = lhs[i].Multiply(rhs[i]);
	}
}