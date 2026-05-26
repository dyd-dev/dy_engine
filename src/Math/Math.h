#pragma once
#include <cstddef> // size_t
#include <cmath>

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
	struct alignas(8) float2
	{
		union
		{
			struct
			{
				float x, y;
			};
			float data[2];
		};

		inline float2() = default;
		inline float2(float _x, float _y) : x(_x), y(_y) {}
		[[nodiscard]] inline float& operator[](size_t index) { return data[index]; }
		[[nodiscard]] inline const float& operator[](size_t index) const { return data[index]; }
	};
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
		[[nodiscard]] inline float& operator[](size_t index) { return data[index]; }
		[[nodiscard]] inline const float& operator[](size_t index) const { return data[index]; }
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
		[[nodiscard]] inline float& operator[](size_t index) { return data[index]; }
		[[nodiscard]] inline const float& operator[](size_t index) const { return data[index]; }
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

	[[nodiscard]] inline float3 operator+(const float3& lhs, const float3& rhs)
	{
		return float3(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z);
	}

	[[nodiscard]] inline float3 operator-(const float3& lhs, const float3& rhs)
	{
		return float3(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z);
	}

	[[nodiscard]] inline float3 operator*(const float3& value, float scale)
	{
		return float3(value.x * scale, value.y * scale, value.z * scale);
	}

	[[nodiscard]] inline float3 operator*(float scale, const float3& value)
	{
		return value * scale;
	}

	[[nodiscard]] inline float Dot(const float3& lhs, const float3& rhs)
	{
		return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
	}

	[[nodiscard]] inline float3 Cross(const float3& lhs, const float3& rhs)
	{
		return float3(
			lhs.y * rhs.z - lhs.z * rhs.y,
			lhs.z * rhs.x - lhs.x * rhs.z,
			lhs.x * rhs.y - lhs.y * rhs.x);
	}

	[[nodiscard]] inline float LengthSquared(const float3& value)
	{
		return Dot(value, value);
	}

	[[nodiscard]] inline float Length(const float3& value)
	{
		return std::sqrt(LengthSquared(value));
	}

	[[nodiscard]] inline float3 NormalizeOr(const float3& value, const float3& fallback)
	{
		const float lengthSquared = LengthSquared(value);
		if(lengthSquared <= 1.0e-8f) return fallback;
		return value * (1.0f / std::sqrt(lengthSquared));
	}

	[[nodiscard]] inline float3 Normalize(const float3& value)
	{
		return NormalizeOr(value, float3(0.0f, 0.0f, 0.0f));
	}

	[[nodiscard]] inline float4x4 MultiplyColumnMajor(const float4x4& lhs, const float4x4& rhs)
	{
		float4x4 result = {};
		for(int column = 0; column < 4; ++column)
		{
			for(int row = 0; row < 4; ++row)
			{
				float value = 0.0f;
				for(int k = 0; k < 4; ++k)
				{
					value += lhs.m[k * 4 + row] * rhs.m[column * 4 + k];
				}
				result.m[column * 4 + row] = value;
			}
		}
		return result;
	}

	[[nodiscard]] inline float4x4 LookAtRH(const float3& eye, const float3& target, const float3& up)
	{
		const float3 forward = NormalizeOr(target - eye, float3(0.0f, 0.0f, -1.0f));
		const float3 right = NormalizeOr(Cross(forward, up), float3(1.0f, 0.0f, 0.0f));
		const float3 cameraUp = Cross(right, forward);

		float4x4 view = float4x4::Identity();
		view.m[0] = right.x;
		view.m[4] = right.y;
		view.m[8] = right.z;
		view.m[12] = -Dot(right, eye);
		view.m[1] = cameraUp.x;
		view.m[5] = cameraUp.y;
		view.m[9] = cameraUp.z;
		view.m[13] = -Dot(cameraUp, eye);
		view.m[2] = -forward.x;
		view.m[6] = -forward.y;
		view.m[10] = -forward.z;
		view.m[14] = Dot(forward, eye);
		return view;
	}

	[[nodiscard]] inline float4x4 LookAtLH(const float3& eye, const float3& target, const float3& up)
	{
		const float3 forward = NormalizeOr(target - eye, float3(0.0f, 0.0f, 1.0f));
		const float3 right = NormalizeOr(Cross(up, forward), float3(1.0f, 0.0f, 0.0f));
		const float3 cameraUp = Cross(forward, right);

		float4x4 view = float4x4::Identity();
		view.m[0] = right.x;
		view.m[4] = right.y;
		view.m[8] = right.z;
		view.m[12] = -Dot(right, eye);
		view.m[1] = cameraUp.x;
		view.m[5] = cameraUp.y;
		view.m[9] = cameraUp.z;
		view.m[13] = -Dot(cameraUp, eye);
		view.m[2] = forward.x;
		view.m[6] = forward.y;
		view.m[10] = forward.z;
		view.m[14] = -Dot(forward, eye);
		return view;
	}

	[[nodiscard]] inline float4x4 OrthographicRH_ZO(float width, float height, float nearPlane, float farPlane, bool flipY = true)
	{
		float4x4 projection = float4x4::Identity();
		projection.m[0] = 2.0f / width;
		projection.m[5] = (flipY ? -2.0f : 2.0f) / height;
		projection.m[10] = 1.0f / (nearPlane - farPlane);
		projection.m[14] = nearPlane / (nearPlane - farPlane);
		return projection;
	}

	[[nodiscard]] inline float4x4 OrthographicLH_ZO(float width, float height, float nearPlane, float farPlane)
	{
		float4x4 projection = float4x4::Identity();
		projection.m[0] = 2.0f / width;
		projection.m[5] = 2.0f / height;
		projection.m[10] = 1.0f / (farPlane - nearPlane);
		projection.m[14] = -nearPlane / (farPlane - nearPlane);
		return projection;
	}

	inline void MultiplyMatricesBatch(const float4x4* lhs, const float4x4* rhs, float4x4* out, size_t count)
	{
		// DOD approach: iterate linearly through aligned memory.
		// Hardware prefetchers will aggressively cache these arrays.
		for(size_t i = 0; i < count; ++i)
			out[i] = lhs[i].Multiply(rhs[i]);
	}
}
