#include "dyf/Scene.h"
#include <cmath>
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

namespace dyf
{
// Scene이 입력을 소유하고, 읽는 쪽에는 변경 가능한 저장소를 전달하지 않는다.
MaterialID Scene::CreateMaterial(const MaterialDesc& material)
{
    m_materials.push_back(material);
    return static_cast<MaterialID>(m_materials.size() - 1);
}

MeshID Scene::CreateMesh(const MeshData& mesh)
{
    m_meshes.push_back(std::make_shared<const MeshData>(mesh));
    return static_cast<MeshID>(m_meshes.size() - 1);
}

EntityID Scene::CreateEntity(MeshID mesh, MaterialID material,
    const Math::float4x4& transform, const EntityLightingDesc& lighting)
{
    const auto count = m_entityMeshes.size();
    try
    {
        m_entityMeshes.push_back(mesh);
        m_entityMaterials.push_back(material);
        m_entityTransforms.push_back(Transform{transform});
        m_entityLighting.push_back(lighting);
        m_entityTransformRevisions.push_back(m_transformRevision+1);
    }
    catch(...)
    {
        m_entityMeshes.resize(count);
        m_entityMaterials.resize(count);
        m_entityTransforms.resize(count);
        m_entityLighting.resize(count);
        m_entityTransformRevisions.resize(count);
        throw;
    }
    ++m_transformRevision;
    return static_cast<EntityID>(count);
}

uint32_t Scene::GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }
EntityHandle Scene::GetEntity(EntityID entity)
{
    if(!IsValid(entity) || ToIndex(entity) >= GetEntityCount())
    { std::fprintf(stderr, "dyf: invalid entity ID.\n"); return {}; }
    EntityHandle handle;
    handle.m_scene = m_lifetime;
    handle.m_id = entity;
    return handle;
}
MeshID Scene::GetEntityMesh(EntityID entity) const { return m_entityMeshes[ToIndex(entity)]; }
MaterialID Scene::GetEntityMaterial(EntityID entity) const { return m_entityMaterials[ToIndex(entity)]; }
const Transform& Scene::GetTransform(EntityID entity) const { return m_entityTransforms[ToIndex(entity)]; }
const EntityLightingDesc& Scene::GetEntityLighting(EntityID entity) const { return m_entityLighting[ToIndex(entity)]; }
void Scene::SetEntityLighting(EntityID entity, const EntityLightingDesc& lighting) { m_entityLighting[ToIndex(entity)] = lighting; }
void Scene::SetMaterial(MaterialID material, const MaterialDesc& value)
{
    static_assert(std::is_nothrow_move_assignable_v<MaterialDesc>);
    auto replacement=value;
    m_materials[ToIndex(material)]=std::move(replacement);
}

const std::vector<std::shared_ptr<const MeshData>>& Scene::Meshes() const { return m_meshes; }
const std::vector<MaterialDesc>& Scene::Materials() const { return m_materials; }
const std::vector<DirectionalLight>& Scene::DirectionalLights() const { return m_directionalLights; }
const std::vector<PointLight>& Scene::PointLights() const { return m_pointLights; }
const std::vector<SpotLight>& Scene::SpotLights() const { return m_spotLights; }
const std::vector<RectAreaLight>& Scene::RectAreaLights() const { return m_rectAreaLights; }
const std::vector<DiscAreaLight>& Scene::DiscAreaLights() const { return m_discAreaLights; }

namespace
{
    bool InvalidHandle(const char* kind) { std::fprintf(stderr, "dyf: invalid %s handle.\n", kind); return false; }
    bool ValidEntity(const std::shared_ptr<Scene>& data, EntityID id)
    { return data && IsValid(id) && ToIndex(id)<data->GetEntityCount(); }
    bool Finite(const Math::float4x4& value)
    { for(float v:value.m) if(!std::isfinite(v)) return false; return true; }

}
// Scene 자신이 수명 표식을 보유한다. Handle의 weak_ptr은 Scene의 생존 기간을 늘리지 않는다.
Scene::Scene() : m_lifetime(this, [](Scene*) {}) {}
Scene::~Scene() = default;
EntityHandle Scene::Add(const MeshData& mesh,const MaterialDesc& material,const Math::float4x4& transform)
{
    const auto meshCount=m_meshes.size(), materialCount=m_materials.size();
    try
    {
        if(mesh.vertices.empty() || mesh.indices.empty() || !Finite(transform))
        { std::fprintf(stderr,"dyf: Scene::Add requires geometry and a finite transform.\n"); return {}; }
        for(auto index:mesh.indices) if(index>=mesh.vertices.size())
        { std::fprintf(stderr,"dyf: mesh index is outside its vertex data.\n"); return {}; }
        EntityHandle handle;handle.m_scene=m_lifetime;
        handle.m_id=this->CreateEntity(this->CreateMesh(mesh),this->CreateMaterial(material),transform);
        return handle;
    }
    catch(const std::exception& error)
    {
        m_meshes.resize(meshCount); m_materials.resize(materialCount);
        std::fprintf(stderr,"dyf: Scene::Add: %s\n",error.what());return {};
    }
}
EntityHandle Scene::AddInstance(const EntityHandle& source,const Math::float4x4& transform)
{
    const auto owner=source.m_scene.lock();
    if(owner.get()!=this || !ValidEntity(owner,source.m_id) || !Finite(transform))
    { std::fprintf(stderr,"dyf: AddInstance requires a live local entity and finite transform.\n"); return {}; }
    const auto count=m_materials.size();
    try
    {
        // Copy before vector growth: the source material may be inside m_materials.
        const auto material=m_materials[ToIndex(GetEntityMaterial(source.m_id))];
        const auto lighting=GetEntityLighting(source.m_id);
        const auto id=CreateMaterial(material);
        return GetEntity(CreateEntity(GetEntityMesh(source.m_id),id,transform,lighting));
    }
    catch(const std::exception& error)
    { m_materials.resize(count);std::fprintf(stderr,"dyf: AddInstance: %s\n",error.what());return {}; }
}
EntityHandle::operator bool() const { const auto data=m_scene.lock();return ValidEntity(data,m_id); }
Math::float4x4 EntityHandle::GetTransform() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return Math::float4x4::Identity();}return data->GetTransform(m_id).worldMatrix; }
MaterialDesc EntityHandle::GetMaterial() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return {};}return data->Materials()[ToIndex(data->GetEntityMaterial(m_id))]; }
bool EntityHandle::SetTransform(const Math::float4x4& value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity");
    if(!Finite(value)){std::fprintf(stderr,"dyf: transform must be finite.\n");return false;}
    auto& current=data->m_entityTransforms[ToIndex(m_id)].worldMatrix;
    if(std::equal(std::begin(current.m),std::end(current.m),std::begin(value.m)))return true;
    current=value;
    data->m_entityTransformRevisions[ToIndex(m_id)]=++data->m_transformRevision;
    return true;
}
bool EntityHandle::SetPosition(Math::float3 value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity");
    auto transform=data->GetTransform(m_id).worldMatrix;transform.m[12]=value.x;transform.m[13]=value.y;transform.m[14]=value.z;
    return SetTransform(transform);
}
bool EntityHandle::SetMaterial(const MaterialDesc& value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity");
    try {data->SetMaterial(data->GetEntityMaterial(m_id),value);return true;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: material update: %s\n",error.what());return false;}
}
EntityLightingDesc EntityHandle::GetLighting() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return {};}return data->GetEntityLighting(m_id); }
bool EntityHandle::SetLighting(const EntityLightingDesc& lighting)
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity");data->SetEntityLighting(m_id,lighting);return true; }
template<typename T> std::vector<T>& Scene::Lights()
{
    if constexpr(std::is_same_v<T,DirectionalLight>)return m_directionalLights;
    else if constexpr(std::is_same_v<T,PointLight>)return m_pointLights;
    else if constexpr(std::is_same_v<T,SpotLight>)return m_spotLights;
    else if constexpr(std::is_same_v<T,RectAreaLight>)return m_rectAreaLights;
    else return m_discAreaLights;
}
template<typename T> LightHandle<T>::operator bool() const
{const auto data=m_scene.lock();return data && m_index<data->template Lights<T>().size();}
template<typename T> T LightHandle<T>::Get() const
{const auto data=m_scene.lock();if(!data || m_index>=data->template Lights<T>().size()){InvalidHandle("light");return {};}return data->template Lights<T>()[m_index];}
template<typename T> bool LightHandle<T>::Set(const T& value)
{const auto data=m_scene.lock();if(!data || m_index>=data->template Lights<T>().size())return InvalidHandle("light");data->template Lights<T>()[m_index]=value;return true;}

LightHandle<DirectionalLight> Scene::Add(const DirectionalLight& light)
{
    try {LightHandle<DirectionalLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<DirectionalLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<DirectionalLight>;

LightHandle<PointLight> Scene::Add(const PointLight& light)
{
    try {LightHandle<PointLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<PointLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<PointLight>;

LightHandle<SpotLight> Scene::Add(const SpotLight& light)
{
    try {LightHandle<SpotLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<SpotLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<SpotLight>;

LightHandle<RectAreaLight> Scene::Add(const RectAreaLight& light)
{
    try {LightHandle<RectAreaLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<RectAreaLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<RectAreaLight>;

LightHandle<DiscAreaLight> Scene::Add(const DiscAreaLight& light)
{
    try {LightHandle<DiscAreaLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<DiscAreaLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<DiscAreaLight>;
}
