#pragma once

#include <cstdint>
#include <memory>

#include "dyf/Math/Math.h"
#include "dyf/Types.h"

namespace dyf
{
	class Scene;
	// 객체의 조명 참여 설정이다. 현재는 그림자 생성과 수신을 제어하며, 표면 반응은 MaterialDesc가 담당한다.
	struct EntityLightingDesc
	{
		bool castShadow = true;
		bool receiveShadow = true;
	};

	struct DirectionalLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 direction = Math::float3(0.35f, 0.65f, 0.68f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 4.0f;
		bool castShadow = true;
		float shadowStrength = 0.45f;
	};

	struct PointLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float range = 6.0f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 6.0f;
		bool castShadow = true;
		float shadowStrength = 0.5f;
	};

	struct SpotLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float range = 6.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float outerConeRadians = 0.52359878f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
		float intensity = 6.0f;
		float innerConeRadians = 0.34906585f;
		bool castShadow = false;
		float shadowStrength = 1.0f;
	};

	struct RectAreaLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float intensity = 100.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float width = 1.0f;
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		float height = 1.0f;
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
	};

	struct DiscAreaLight
	{
		bool enabled = true;
		int32_t priority = 0;
		Math::float3 position = Math::float3(0.0f, 0.0f, 2.0f);
		float intensity = 100.0f;
		Math::float3 direction = Math::float3(0.0f, 0.0f, -1.0f);
		float radius = 0.5f;
		Math::float3 up = Math::float3(0.0f, 1.0f, 0.0f);
		Math::float3 color = Math::float3(1.0f, 0.94f, 0.82f);
	};


	// 광원은 Scene이 소유한다. 다른 객체를 추가해도 이 Handle의 대상은 유지된다.
	template <typename LightType>
	struct LightHandle
	{
		LightHandle() = default;
		explicit operator bool() const;
		[[nodiscard]] LightType Get() const;
		bool Set(const LightType& light);
	private:
		friend class Scene;
		std::weak_ptr<Scene> m_scene;
		uint32_t m_index = UINT32_MAX;
	};
}
