#pragma once
#include "Math/Math.h"

namespace dy::Graphics
{
	class Camera
	{
	public:
		Camera();

		void SetLookAt(const Math::Vector3& position, const Math::Vector3& target, const Math::Vector3& up);
		void SetPerspective(float fovY, float aspect, float nearZ, float farZ);

		void UpdateMatrices();

		[[nodiscard]] const Math::Matrix4x4& GetViewProjectionMatrix() const
		{
			return m_viewProjectionMatrix;
		}

	private:
		Math::Vector3 m_position;
		Math::Vector3 m_target;
		Math::Vector3 m_up;

		float m_fov;
		float m_aspectRatio;
		float m_nearPlane;
		float m_farPlane;

		Math::Matrix4x4 m_viewMatrix;
		Math::Matrix4x4 m_projectionMatrix;
		Math::Matrix4x4 m_viewProjectionMatrix;

		bool m_isDirty;
	};
}