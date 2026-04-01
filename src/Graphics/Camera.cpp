#include "Camera.h"

using namespace dy::Graphics;

Camera::Camera()
	: m_viewMatrix(Math::Matrix4x4::Identity()),
	m_projectionMatrix(Math::Matrix4x4::Identity()),
	m_viewProjectionMatrix(Math::Matrix4x4::Identity()),
	m_position(0.0f, 0.0f, -10.0f),
	m_target(0.0f, 0.0f, 0.0f),
	m_up(0.0f, 1.0f, 0.0f),
	m_isDirty(true)
{
}

void Camera::SetLookAt(const Math::Vector3& position, const Math::Vector3& target, const Math::Vector3& up)
{
	m_position = position;
	m_target = target;
	m_up = up;
	m_isDirty = true;
}

void Camera::SetPerspective(float fovY, float aspect, float nearPlane, float farPlane)
{
	m_projectionMatrix = Math::Matrix4x4::CreatePerspectiveFovLH(fovY, aspect, nearPlane, farPlane);
	m_isDirty = true;
}

void Camera::UpdateMatrices()
{
	if(!m_isDirty) return;

	m_viewMatrix = Math::Matrix4x4::CreateLookAtLH(m_position, m_target, m_up);
	m_viewProjectionMatrix = m_viewMatrix.Multiply(m_projectionMatrix);

	m_isDirty = false;
}