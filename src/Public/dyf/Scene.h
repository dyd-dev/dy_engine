#pragma once
#include <memory>
#include <vector>
#include "dyf/Types.h"
#include "dyf/Light.h"
#include "dyf/Material.h"
#include "dyf/Mesh.h"

namespace dyf
{
    class Scene;
	// 객체 데이터는 Scene이 소유한다. Scene이 소멸하면 Handle도 만료된다.
	struct EntityHandle
	{
		EntityHandle() = default;
		explicit operator bool() const;

		[[nodiscard]] Math::float4x4 GetTransform() const;
		[[nodiscard]] MaterialDesc GetMaterial() const;

		bool SetTransform(const Math::float4x4& transform);
		bool SetPosition(Math::float3 position);
		bool SetMaterial(const MaterialDesc& material);
		[[nodiscard]] EntityLightingDesc GetLighting() const;
		bool SetLighting(const EntityLightingDesc& lighting = {});
	private:
		friend class Scene;
		std::weak_ptr<Scene> m_scene;
		EntityID m_id = EntityID::Invalid;
	};


    // CPU 장면을 소유한다. Renderer와 RHI 사용자는 같은 읽기 API로 입력을 얻는다.
    class Scene
    {
    public:
        Scene();
        virtual ~Scene();
        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;

        // 메시와 재질을 복사해 소유한다. 생략한 재질과 변환에는 기본값을 사용한다.
        [[nodiscard]] EntityHandle Add(const MeshData& mesh, const MaterialDesc& material = {},
            const Math::float4x4& transform = Math::float4x4::Identity());
        [[nodiscard]] LightHandle<DirectionalLight> Add(const DirectionalLight& light);
        [[nodiscard]] LightHandle<PointLight> Add(const PointLight& light);
        [[nodiscard]] LightHandle<SpotLight> Add(const SpotLight& light);
        [[nodiscard]] LightHandle<RectAreaLight> Add(const RectAreaLight& light);
        [[nodiscard]] LightHandle<DiscAreaLight> Add(const DiscAreaLight& light);

        // 객체 ID는 0부터 GetEntityCount()-1까지다. 변경은 Handle을 통해 수행한다.
        [[nodiscard]] uint32_t GetEntityCount() const;
        [[nodiscard]] EntityHandle GetEntity(EntityID entity);
        [[nodiscard]] MeshID GetEntityMesh(EntityID entity) const;
        [[nodiscard]] MaterialID GetEntityMaterial(EntityID entity) const;
        [[nodiscard]] const Transform& GetTransform(EntityID entity) const;
        [[nodiscard]] const EntityLightingDesc& GetEntityLighting(EntityID entity) const;

        // ID는 해당 컬렉션의 인덱스다. 메시 입력은 등록 후 변경하지 않는다.
        // 공유 메시의 수명은 Scene과 독립적이며, GPU 캐시는 이 입력 자체로 식별할 수 있다.
        [[nodiscard]] const std::vector<std::shared_ptr<const MeshData>>& Meshes() const;
        [[nodiscard]] const std::vector<MaterialDesc>& Materials() const;
        [[nodiscard]] const std::vector<DirectionalLight>& DirectionalLights() const;
        [[nodiscard]] const std::vector<PointLight>& PointLights() const;
        [[nodiscard]] const std::vector<SpotLight>& SpotLights() const;
        [[nodiscard]] const std::vector<RectAreaLight>& RectAreaLights() const;
        [[nodiscard]] const std::vector<DiscAreaLight>& DiscAreaLights() const;

    protected:
        // 확장에서 여러 객체가 같은 메시·재질을 공유해 등록할 때 사용하는 경계다.
        MaterialID CreateMaterial(const MaterialDesc& material = {});
        MeshID CreateMesh(const MeshData& mesh);
        EntityID CreateEntity(MeshID mesh, MaterialID material,
            const Math::float4x4& transform = Math::float4x4::Identity(),
            const EntityLightingDesc& lighting = {});

    private:
        friend struct EntityHandle;
        template<typename T> friend struct LightHandle;
        template<typename T> std::vector<T>& Lights();
        void SetMaterial(MaterialID material, const MaterialDesc& value);
        void SetEntityLighting(EntityID entity, const EntityLightingDesc& lighting);

        std::vector<std::shared_ptr<const MeshData>> m_meshes;
        std::vector<MaterialDesc> m_materials;
        std::vector<MeshID> m_entityMeshes;
        std::vector<MaterialID> m_entityMaterials;
        std::vector<Transform> m_entityTransforms;
        std::vector<EntityLightingDesc> m_entityLighting;
        std::vector<DirectionalLight> m_directionalLights;
        std::vector<PointLight> m_pointLights;
        std::vector<SpotLight> m_spotLights;
        std::vector<RectAreaLight> m_rectAreaLights;
        std::vector<DiscAreaLight> m_discAreaLights;
        // 객체·광원 Handle의 만료 판정만 담당한다. Renderer에는 전달하지 않는다.
        std::shared_ptr<Scene> m_lifetime;
    };
}
