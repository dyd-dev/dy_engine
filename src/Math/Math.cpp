#include "Math.h"
#include <cmath>
#include <immintrin.h> // Includes all SSE/AVX intrinsics

namespace dy::Math
{
	Vector3 Vector3::Normalize() const
	{
		float length = std::sqrt(x*x + y*y + z*z);
		if(length > 0.0f)
		{
			float invLength = 1.0f / length;
			return Vector3(x*invLength, y*invLength, z*invLength);
		}
		return *this;
	}
	Vector3 Vector3::Cross(const Vector3& rhs) const
	{
		return Vector3(
			y * rhs.z - z * rhs.y,
			z * rhs.x - x * rhs.z,
			x * rhs.y - y * rhs.x
		);
	}
	float Vector3::Dot(const Vector3& rhs) const
	{
		return x * rhs.x + y * rhs.y + z * rhs.z;
	}

	Vector3 Vector3::operator+(const Vector3& rhs) const
	{
		return Vector3(x + rhs.x, y + rhs.y, z + rhs.z);
	}
	Vector3 Vector3::operator-(const Vector3& rhs) const
	{
		return Vector3(x - rhs.x, y - rhs.y, z - rhs.z);
	}

	Matrix4x4 Matrix4x4::Identity()
	{
		Matrix4x4 res = {};
		for(int i=0; i<16; i++) res.m[i] = 0.0f;
		res.m[0] = 1.0f; res.m[5] = 1.0f; res.m[10] = 1.0f; res.m[15] = 1.0f;
		return res;
	}
	// Standard Left-Handed LookAt implementation
	Matrix4x4 Matrix4x4::CreateLookAtLH(const Vector3& eye, const Vector3& target, const Vector3& up)
	{
		Vector3 zAxis = (target - eye).Normalize();
		Vector3 xAxis = up.Cross(zAxis).Normalize();
		Vector3 yAxis = zAxis.Cross(xAxis);

		Matrix4x4 res = Identity();
		res.m[0] = xAxis.x; res.m[4] = xAxis.y; res.m[8]  = xAxis.z; res.m[12] = -xAxis.Dot(eye);
		res.m[1] = yAxis.x; res.m[5] = yAxis.y; res.m[9]  = yAxis.z; res.m[13] = -yAxis.Dot(eye);
		res.m[2] = zAxis.x; res.m[6] = zAxis.y; res.m[10] = zAxis.z; res.m[14] = -zAxis.Dot(eye);
		res.m[3] = 0.0f;    res.m[7] = 0.0f;    res.m[11] = 0.0f;    res.m[15] = 1.0f;
		return res;
	}
	// Standard Left-Handed Perspective projection
	Matrix4x4 Matrix4x4::CreatePerspectiveFovLH(float fovY, float aspect, float nearZ, float farZ)
	{
		float yScale = 1.0f / std::tan(fovY * 0.5f);
		float xScale = yScale / aspect;

		Matrix4x4 res = {};
		for(int i=0; i<16; i++) res.m[i] = 0.0f;

		res.m[0] = xScale;
		res.m[5] = yScale;
		res.m[10] = 1.0f;
		res.m[14] = -(nearZ * farZ) / (farZ - nearZ);
		return res;
	}

	Matrix4x4 Matrix4x4::Multiply(const Matrix4x4& rhs) const
	{
		Matrix4x4 res = {};
		for (int i = 0; i < 4; ++i) 
		{
			for (int j = 0; j < 4; ++j) 
			{
				res.m[i * 4 + j] =
					m[i * 4 + 0] * rhs.m[0 * 4 + j] +
					m[i * 4 + 1] * rhs.m[1 * 4 + j] +
					m[i * 4 + 2] * rhs.m[2 * 4 + j] +
					m[i * 4 + 3] * rhs.m[3 * 4 + j];
			}
		}
		return res;
	}

	Matrix4x4 Matrix4x4::MultiplySIMD(const Matrix4x4& rhs) const
	{
		Matrix4x4 res = {};
		for (int i = 0; i < 4; ++i) 
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
		return res;
	}

	void MultiplyMatricesBatch(const Matrix4x4* lhs, const Matrix4x4* rhs, Matrix4x4* out, size_t count)
	{
		// DOD approach: iterate linearly through aligned memory.
		// Hardware prefetchers will aggressively cache these arrays.
		for (size_t i = 0; i < count; ++i)
			 out[i] = lhs[i].Multiply(rhs[i]);
	}
}