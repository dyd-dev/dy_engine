#pragma once
#include <algorithm>
#include <cstddef> // size_t
#include <cmath>

// SIMD 는 DY_SIMD_ENABLED 가 정의됐고(빌드의 DY_ENABLE_SIMD=ON) 아키텍처가 지원할 때만 켜진다.
// 정의가 없으면 모든 경로가 스칼라 폴백으로 동작한다(결과는 동일, 속도만 차이).
#if defined(DY_SIMD_ENABLED)
	#if defined(__x86_64__) || defined(_M_X64)
		#define DY_SIMD_X64
		#include <immintrin.h> // SSE/AVX
	#elif defined(__aarch64__) || defined(_M_ARM64)
		#define DY_SIMD_ARM64
		#include <arm_neon.h> // ARM NEON
	#endif
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
#else
			// 스칼라 폴백: SIMD 경로와 동일한 곱(res.col_i[r] = Σ_k m[i*4+k]*rhs.m[k*4+r]).
			for(int i = 0; i < 4; ++i)
			{
				for(int r = 0; r < 4; ++r)
				{
					float value = 0.0f;
					for(int k = 0; k < 4; ++k) value += m[i * 4 + k] * rhs.m[k * 4 + r];
					res.m[i * 4 + r] = value;
				}
			}
#endif
			return res;
		}
	};

	// 열-주도 행렬 곱 a*b. 기존 SIMD 경로(float4x4::Multiply)를 재사용한다.
	// x.Multiply(y) 는 열-주도 의미에서 y*x 를 계산하므로, a*b = b.Multiply(a) 이다.
	[[nodiscard]] inline float4x4 operator*(const float4x4& a, const float4x4& b)
	{
		return b.Multiply(a);
	}

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

	[[nodiscard]] inline float3 TransformPoint(const float4x4& matrix, const float3& point)
	{
		return float3(
			matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
			matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
			matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14]);
	}

	[[nodiscard]] inline float3 TransformVector(const float4x4& matrix, const float3& vector)
	{
		return float3(
			matrix.m[0] * vector.x + matrix.m[4] * vector.y + matrix.m[8] * vector.z,
			matrix.m[1] * vector.x + matrix.m[5] * vector.y + matrix.m[9] * vector.z,
			matrix.m[2] * vector.x + matrix.m[6] * vector.y + matrix.m[10] * vector.z);
	}

	// ===== 변환 빌더 (열-주도 저장: 평행이동 = m[12..14]) =====
	// 합성은 operator* 로: world = Translation(t) * RotationZ(yaw) * Scaling(s).

	[[nodiscard]] inline float4x4 Translation(const float3& t)
	{
		float4x4 m = float4x4::Identity();
		m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
		return m;
	}

	[[nodiscard]] inline float4x4 Scaling(const float3& s)
	{
		float4x4 m = float4x4::Identity();
		m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z;
		return m;
	}

	[[nodiscard]] inline float4x4 Scaling(float s)
	{
		return Scaling(float3(s, s, s));
	}

	[[nodiscard]] inline float4x4 RotationX(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		float4x4 m = float4x4::Identity();
		m.m[5] = c; m.m[6] = s; m.m[9] = -s; m.m[10] = c;
		return m;
	}

	[[nodiscard]] inline float4x4 RotationY(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		float4x4 m = float4x4::Identity();
		m.m[0] = c; m.m[2] = -s; m.m[8] = s; m.m[10] = c;
		return m;
	}

	[[nodiscard]] inline float4x4 RotationZ(float radians)
	{
		const float c = std::cos(radians), s = std::sin(radians);
		float4x4 m = float4x4::Identity();
		m.m[0] = c; m.m[1] = s; m.m[4] = -s; m.m[5] = c;
		return m;
	}

	// 임의 축 회전(Rodrigues). axis 는 내부에서 정규화.
	[[nodiscard]] inline float4x4 RotationAxis(const float3& axis, float radians)
	{
		const float3 a = NormalizeOr(axis, float3(0.0f, 0.0f, 1.0f));
		const float c = std::cos(radians), s = std::sin(radians), t = 1.0f - c;
		float4x4 m = float4x4::Identity();
		m.m[0] = c + a.x * a.x * t;        m.m[1] = a.y * a.x * t + a.z * s;  m.m[2] = a.z * a.x * t - a.y * s;
		m.m[4] = a.x * a.y * t - a.z * s;  m.m[5] = c + a.y * a.y * t;        m.m[6] = a.z * a.y * t + a.x * s;
		m.m[8] = a.x * a.z * t + a.y * s;  m.m[9] = a.y * a.z * t - a.x * s;  m.m[10] = c + a.z * a.z * t;
		return m;
	}

	struct Bounds3
	{
		float3 min = float3(0.0f, 0.0f, 0.0f);
		float3 max = float3(0.0f, 0.0f, 0.0f);
		bool valid = false;

		void Include(const float3& point)
		{
			if(!valid)
			{
				min = point;
				max = point;
				valid = true;
				return;
			}

			min.x = std::min(min.x, point.x);
			min.y = std::min(min.y, point.y);
			min.z = std::min(min.z, point.z);
			max.x = std::max(max.x, point.x);
			max.y = std::max(max.y, point.y);
			max.z = std::max(max.z, point.z);
		}

		[[nodiscard]] float3 Center() const
		{
			return float3(
				(min.x + max.x) * 0.5f,
				(min.y + max.y) * 0.5f,
				(min.z + max.z) * 0.5f);
		}

		[[nodiscard]] float3 Extent() const
		{
			return float3(
				(max.x - min.x) * 0.5f,
				(max.y - min.y) * 0.5f,
				(max.z - min.z) * 0.5f);
		}
	};

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

	// RHI 클립공간은 Y-up, Z [0, 1]이다. Backend가 native 좌표계로 번역하며 투영 함수는 Backend를 모른다.
	[[nodiscard]] inline float4x4 OrthographicRH_ZO(float width, float height, float nearPlane, float farPlane)
	{
		float4x4 projection = float4x4::Identity();
		projection.m[0] = 2.0f / width;
		projection.m[5] = 2.0f / height;
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

	[[nodiscard]] inline float4x4 PerspectiveRH_ZO(float fovYRadians, float aspect, float nearPlane, float farPlane)
	{
		const float safeFov = std::max(std::min(fovYRadians, 3.0f), 0.1f);
		const float safeAspect = std::max(aspect, 0.0001f);
		const float f = 1.0f / std::tan(safeFov * 0.5f);

		float4x4 projection = {};
		projection.m[0] = f / safeAspect;
		projection.m[5] = f;
		projection.m[10] = farPlane / (nearPlane - farPlane);
		projection.m[11] = -1.0f;
		projection.m[14] = (nearPlane * farPlane) / (nearPlane - farPlane);
		return projection;
	}

	[[nodiscard]] inline float4x4 PerspectiveLH_ZO(float fovYRadians, float aspect, float nearPlane, float farPlane)
	{
		const float safeFov = std::max(std::min(fovYRadians, 3.0f), 0.1f);
		const float safeAspect = std::max(aspect, 0.0001f);
		const float f = 1.0f / std::tan(safeFov * 0.5f);

		float4x4 projection = {};
		projection.m[0] = f / safeAspect;
		projection.m[5] = f;
		projection.m[10] = farPlane / (farPlane - nearPlane);
		projection.m[11] = 1.0f;
		projection.m[14] = -(nearPlane * farPlane) / (farPlane - nearPlane);
		return projection;
	}

	// ===== Quaternion (x, y, z, w) =====
	// 단일 쿼터니언 연산은 스칼라로 둔다. AoS 4레인 SIMD 는 셔플 오버헤드가 커서
	// 단일 곱에선 이득이 거의 없고, 진짜 이득은 대량을 SoA 로 배치 처리할 때 나온다.
	struct alignas(16) quat
	{
		float x, y, z, w;

		inline quat() = default;
		inline quat(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

		[[nodiscard]] static inline quat Identity() { return quat(0.0f, 0.0f, 0.0f, 1.0f); }
	};

	[[nodiscard]] inline quat QuatFromAxisAngle(const float3& axis, float radians)
	{
		const float3 a = NormalizeOr(axis, float3(0.0f, 0.0f, 1.0f));
		const float half = radians * 0.5f;
		const float s = std::sin(half);
		return quat(a.x * s, a.y * s, a.z * s, std::cos(half));
	}

	[[nodiscard]] inline float Dot(const quat& a, const quat& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	}

	[[nodiscard]] inline quat Normalize(const quat& q)
	{
		const float lengthSquared = Dot(q, q);
		if(lengthSquared <= 1.0e-12f) return quat::Identity();
		const float inv = 1.0f / std::sqrt(lengthSquared);
		return quat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
	}

	// Hamilton 곱 a*b (a 를 적용한 뒤 b 가 아니라, 회전 합성 R(a)·R(b) 의 쿼터니언).
	[[nodiscard]] inline quat operator*(const quat& a, const quat& b)
	{
		return quat(
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
	}

	[[nodiscard]] inline quat Slerp(const quat& from, quat to, float t)
	{
		float dot = Dot(from, to);
		if(dot < 0.0f) { to = quat(-to.x, -to.y, -to.z, -to.w); dot = -dot; }
		if(dot > 0.9995f)
		{
			// 거의 동일 → 선형 보간 후 정규화(수치 안정).
			return Normalize(quat(
				from.x + (to.x - from.x) * t,
				from.y + (to.y - from.y) * t,
				from.z + (to.z - from.z) * t,
				from.w + (to.w - from.w) * t));
		}
		const float theta = std::acos(dot);
		const float sinTheta = std::sin(theta);
		const float wFrom = std::sin((1.0f - t) * theta) / sinTheta;
		const float wTo = std::sin(t * theta) / sinTheta;
		return quat(
			from.x * wFrom + to.x * wTo,
			from.y * wFrom + to.y * wTo,
			from.z * wFrom + to.z * wTo,
			from.w * wFrom + to.w * wTo);
	}

	// 단위 쿼터니언 → 열-주도 회전 행렬.
	[[nodiscard]] inline float4x4 QuatToMatrix(const quat& q)
	{
		const float x = q.x, y = q.y, z = q.z, w = q.w;
		const float xx = x * x, yy = y * y, zz = z * z;
		const float xy = x * y, xz = x * z, yz = y * z;
		const float wx = w * x, wy = w * y, wz = w * z;
		float4x4 m = float4x4::Identity();
		m.m[0] = 1.0f - 2.0f * (yy + zz); m.m[1] = 2.0f * (xy + wz);        m.m[2] = 2.0f * (xz - wy);
		m.m[4] = 2.0f * (xy - wz);        m.m[5] = 1.0f - 2.0f * (xx + zz); m.m[6] = 2.0f * (yz + wx);
		m.m[8] = 2.0f * (xz + wy);        m.m[9] = 2.0f * (yz - wx);        m.m[10] = 1.0f - 2.0f * (xx + yy);
		return m;
	}

	inline void MultiplyMatricesBatch(const float4x4* lhs, const float4x4* rhs, float4x4* out, size_t count)
	{
		// DOD approach: iterate linearly through aligned memory.
		// Hardware prefetchers will aggressively cache these arrays.
		for(size_t i = 0; i < count; ++i)
			out[i] = lhs[i].Multiply(rhs[i]);
	}
}
