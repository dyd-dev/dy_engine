#include "dyf/Scene.h"
#include "../Platform/Profiler/CollectorInternal.h"
#include <cmath>
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

namespace dyf
{
// 재질 사본을 Scene 소유 배열에 추가하고 그 인덱스를 MaterialID로 반환하며, 외부 조회에는 읽기 전용 저장소를 제공한다.
MaterialID Scene::CreateMaterial(const MaterialDesc& material)
{
    m_materials.push_back(material);
    return static_cast<MaterialID>(m_materials.size() - 1);
}

// 메시 사본을 읽기 전용 공유 저장소에 넣는다. 여러 객체는 같은 MeshID로 기하 데이터를 공유한다.
MeshID Scene::CreateMesh(const MeshData& mesh)
{
    m_meshes.push_back(std::make_shared<const MeshData>(mesh));
    return static_cast<MeshID>(m_meshes.size() - 1);
}

// 모든 객체 배열에 같은 인덱스를 추가한다. 중간 예외에는 모든 배열 길이를 되돌린 뒤 예외를 다시 전달한다.
EntityID Scene::CreateEntity(MeshID mesh, MaterialID material,
    const Math::float4x4& transform, const EntityLightingDesc& lighting)
{
    const auto count = m_entityMeshes.size();
    try
    {
        m_entityMeshes.push_back(mesh);
        m_entityMaterials.push_back(material);
        m_entityTransforms.push_back(transform);
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
    // 배열 추가가 모두 성공한 뒤 장면 변경 번호를 확정한다. 관측 객체 등록 실패는 장면 생성 실패로 확장하지 않는다.
    ++m_transformRevision;
    if(Platform::IsLiveCollectorEnabled()){ // 수집 중일 때만 객체 관측 ID를 추가하며 관측 예외는 무시한다.
        try{m_liveEntities.resize(count+1,0);m_liveEntities[count]=Platform::RegisterLiveObject(nullptr,{Platform::LiveObjectKind::Entity,0,m_liveId,1,sizeof(Math::float4x4),"Scene entity"});}catch(...){}
    }
    return static_cast<EntityID>(count);
}

// 객체 배열의 공통 길이가 유효한 EntityID 범위다.
uint32_t Scene::GetEntityCount() const { return static_cast<uint32_t>(m_entityMeshes.size()); }
// ID를 검사해 Scene 수명 표식과 묶은 Handle을 반환하며, 무효 ID나 범위 밖 입력이면 진단 후 빈 Handle을 반환한다.
EntityHandle Scene::GetEntity(EntityID entity)
{
    if(!IsValid(entity) || ToIndex(entity) >= GetEntityCount())
    { std::fprintf(stderr, "dyf: invalid entity ID.\n"); return {}; }
    EntityHandle handle;
    handle.m_scene = m_lifetime;
    handle.m_id = entity;
    return handle;
}
// 아래 직접 조회·수정은 유효한 ID를 전제로 배열에 접근한다. 공개 Handle 경로에서 수명과 범위를 검사한다.
MeshID Scene::GetEntityMesh(EntityID entity) const { return m_entityMeshes[ToIndex(entity)]; }
MaterialID Scene::GetEntityMaterial(EntityID entity) const { return m_entityMaterials[ToIndex(entity)]; }
const Math::float4x4& Scene::GetTransform(EntityID entity) const { return m_entityTransforms[ToIndex(entity)]; }
const EntityLightingDesc& Scene::GetEntityLighting(EntityID entity) const { return m_entityLighting[ToIndex(entity)]; }
void Scene::SetEntityLighting(EntityID entity, const EntityLightingDesc& lighting) { m_entityLighting[ToIndex(entity)] = lighting; }
// 교체할 재질 사본을 먼저 만들고 예외 없는 이동 대입으로 확정한다. 복사가 실패하면 기존 재질은 유지된다.
void Scene::SetMaterial(MaterialID material, const MaterialDesc& value)
{
    static_assert(std::is_nothrow_move_assignable_v<MaterialDesc>);
    auto replacement=value;
    m_materials[ToIndex(material)]=std::move(replacement);
}

// 렌더러에는 읽기 전용 컨테이너 참조를 제공한다. Scene이 실제 저장소를 계속 소유한다.
const std::vector<std::shared_ptr<const MeshData>>& Scene::Meshes() const { return m_meshes; }
const std::vector<MaterialDesc>& Scene::Materials() const { return m_materials; }
const std::vector<DirectionalLight>& Scene::DirectionalLights() const { return m_directionalLights; }
const std::vector<PointLight>& Scene::PointLights() const { return m_pointLights; }
const std::vector<SpotLight>& Scene::SpotLights() const { return m_spotLights; }
const std::vector<RectAreaLight>& Scene::RectAreaLights() const { return m_rectAreaLights; }
const std::vector<DiscAreaLight>& Scene::DiscAreaLights() const { return m_discAreaLights; }

namespace
{
    // 잘못되거나 수명이 끝난 Handle의 공통 오류를 출력하고 false를 돌려준다.
    bool InvalidHandle(const char* kind) { std::fprintf(stderr, "dyf: invalid %s handle.\n", kind); return false; }
    // Scene 표식이 살아 있고 ID가 현재 객체 배열 안에 있는지 함께 검사한다.
    bool ValidEntity(const std::shared_ptr<Scene>& data, EntityID id)
    { return data && IsValid(id) && ToIndex(id)<data->GetEntityCount(); }
    // 행렬의 모든 성분이 유한한지 검사해 NaN·무한대 변환의 유입을 막는다.
    bool Finite(const Math::float4x4& value)
    { for(float v:value.m) if(!std::isfinite(v)) return false; return true; }

}
// Scene 자신이 수명 표식을 보유한다. Handle의 weak_ptr은 Scene의 생존 기간을 늘리지 않는다.
// Scene 자신을 삭제하지 않는 shared_ptr을 수명 표식으로 갖고 관측 객체를 등록한다. Handle은 weak_ptr로 참조한다.
Scene::Scene() : m_lifetime(this, [](Scene*) {}) {m_liveId=Platform::RegisterLiveObject(this,{Platform::LiveObjectKind::Scene,0,0,0,0,"Scene"});}
// 등록된 객체와 장면의 관측 수명을 종료한다. 실제 데이터는 멤버 소멸로 정리된다.
Scene::~Scene() {for(const auto id:m_liveEntities)Platform::RetireLiveObject(id);Platform::RetireLiveObject(m_liveId);}
// 기하·인덱스·변환을 검증하고 메시·재질·객체를 만든다. 예외면 이 호출에서 추가한 메시·재질까지 되돌린다.
EntityHandle Scene::Add(const MeshData& mesh,const MaterialDesc& material,const Math::float4x4& transform)
{
    const auto meshCount=m_meshes.size(), materialCount=m_materials.size();
    try
    {
        if(mesh.vertices.empty() || mesh.indices.empty() || !Finite(transform)) // 기하가 없거나 변환이 비유한 값이면 아무 객체도 추가하지 않는다.
        { std::fprintf(stderr,"dyf: Scene::Add requires geometry and a finite transform.\n"); return {}; }
        for(auto index:mesh.indices) if(index>=mesh.vertices.size()) // 어떤 인덱스든 정점 범위를 벗어나면 입력을 거부한다.
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
// 같은 Scene의 살아 있는 원본에서 메시를 공유하고 재질·조명은 복사해 새 객체를 만든다.
EntityHandle Scene::AddInstance(const EntityHandle& source,const Math::float4x4& transform)
{
    const auto owner=source.m_scene.lock();
    if(owner.get()!=this || !ValidEntity(owner,source.m_id) || !Finite(transform)) // 다른 장면의 Handle·만료된 객체·비유한 변환이면 인스턴스를 만들지 않는다.
    { std::fprintf(stderr,"dyf: AddInstance requires a live local entity and finite transform.\n"); return {}; }
    const auto count=m_materials.size();
    try
    {
        // 벡터 성장 전에 내부의 원본 재질을 복사해 재할당으로 참조가 무효가 되는 것을 막는다.
        const auto material=m_materials[ToIndex(GetEntityMaterial(source.m_id))];
        const auto lighting=GetEntityLighting(source.m_id);
        const auto id=CreateMaterial(material);
        return GetEntity(CreateEntity(GetEntityMesh(source.m_id),id,transform,lighting));
    }
    catch(const std::exception& error)
    { m_materials.resize(count);std::fprintf(stderr,"dyf: AddInstance: %s\n",error.what());return {}; }
}
// Handle 수명 표식과 ID 범위를 확인한다. 객체 복사나 GPU 작업은 하지 않는다.
EntityHandle::operator bool() const { const auto data=m_scene.lock();return ValidEntity(data,m_id); }
// Scene 수명과 ID가 유효하면 변환 사본을 반환하고, 유효하지 않으면 진단 후 항등행렬을 반환한다.
Math::float4x4 EntityHandle::GetTransform() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return Math::float4x4::Identity();}return data->GetTransform(m_id); }
// 재질 사본을 반환하므로 호출자가 수정하려면 SetMaterial로 다시 반영해야 한다.
MaterialDesc EntityHandle::GetMaterial() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return {};}return data->Materials()[ToIndex(data->GetEntityMaterial(m_id))]; } // 유효하지 않은 Handle은 기본 재질을 반환한다.
// 유효한 유한 행렬만 반영한다. 실제 값이 달라졌을 때 해당 객체와 장면의 변경 번호를 함께 갱신한다.
bool EntityHandle::SetTransform(const Math::float4x4& value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity"); // 잘못된 Handle이면 변환을 수정하지 않는다.
    if(!Finite(value)){std::fprintf(stderr,"dyf: transform must be finite.\n");return false;} // NaN·무한대 행렬은 기존 변환을 유지하고 거부한다.
    auto& current=data->m_entityTransforms[ToIndex(m_id)];
    if(std::equal(std::begin(current.m),std::end(current.m),std::begin(value.m)))return true; // 행렬 값이 같으면 변경 번호를 올리지 않고 성공한다.
    current=value;
    data->m_entityTransformRevisions[ToIndex(m_id)]=++data->m_transformRevision;
    return true;
}
// 기존 행렬의 이동 성분만 바꾼 뒤 SetTransform의 검증·변경 번호 경로를 사용한다.
bool EntityHandle::SetPosition(Math::float3 value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity"); // 잘못된 Handle이면 위치를 읽거나 수정하지 않는다.
    auto transform=data->GetTransform(m_id);transform.m[12]=value.x;transform.m[13]=value.y;transform.m[14]=value.z;
    return SetTransform(transform);
}
// Handle을 확인하고 재질을 교체한다. 복사 실패는 진단과 false로 전달한다.
bool EntityHandle::SetMaterial(const MaterialDesc& value)
{
    const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity"); // 잘못된 Handle이면 재질을 교체하지 않는다.
    try {data->SetMaterial(data->GetEntityMaterial(m_id),value);return true;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: material update: %s\n",error.what());return false;}
}
// Scene 수명과 ID를 확인해 조명 설정 사본을 반환하며, 유효하지 않으면 진단 후 기본 설정을 반환한다.
EntityLightingDesc EntityHandle::GetLighting() const
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id)){InvalidHandle("entity");return {};}return data->GetEntityLighting(m_id); }
// Scene 수명과 ID가 유효하면 조명 설정을 교체하고, 유효하지 않으면 진단 후 false를 반환한다.
bool EntityHandle::SetLighting(const EntityLightingDesc& lighting)
{ const auto data=m_scene.lock();if(!ValidEntity(data,m_id))return InvalidHandle("entity");data->SetEntityLighting(m_id,lighting);return true; }
// 광원 타입에 맞는 배열을 컴파일 시점에 선택한다. 각 타입의 인덱스 공간은 서로 독립적이다.
template<typename T> std::vector<T>& Scene::Lights()
{
    if constexpr(std::is_same_v<T,DirectionalLight>)return m_directionalLights; // T가 방향광인 인스턴스에만 방향광 배열 접근을 컴파일한다.
    else if constexpr(std::is_same_v<T,PointLight>)return m_pointLights; // T가 점광원이면 점광원 배열을 선택한다.
    else if constexpr(std::is_same_v<T,SpotLight>)return m_spotLights; // T가 스폿광원이면 스폿광원 배열을 선택한다.
    else if constexpr(std::is_same_v<T,RectAreaLight>)return m_rectAreaLights; // T가 사각 면광원이면 그 배열을 선택하고 나머지 지원 타입은 원형 면광원이다.
    else return m_discAreaLights;
}
// 장면 수명과 해당 타입의 광원 배열 범위를 확인한다.
template<typename T> LightHandle<T>::operator bool() const
{const auto data=m_scene.lock();return data && m_index<data->template Lights<T>().size();}
// 범위 안의 광원만 Handle로 감싼다. 범위 밖은 빈 Handle이다.
template<typename T> LightHandle<T> Scene::GetLight(uint32_t index)
{
    LightHandle<T> handle;
    if(index<Lights<T>().size()) {handle.m_scene=m_lifetime;handle.m_index=index;}
    return handle;
}
// 지원하는 다섯 광원 타입의 GetLight 코드를 이 번역 단위에서 명시적으로 생성한다.
template LightHandle<DirectionalLight> Scene::GetLight<DirectionalLight>(uint32_t);
template LightHandle<PointLight> Scene::GetLight<PointLight>(uint32_t);
template LightHandle<SpotLight> Scene::GetLight<SpotLight>(uint32_t);
template LightHandle<RectAreaLight> Scene::GetLight<RectAreaLight>(uint32_t);
template LightHandle<DiscAreaLight> Scene::GetLight<DiscAreaLight>(uint32_t);
// 장면 수명과 광원 인덱스를 확인해 값을 복사하며, 유효하지 않으면 진단 후 기본값을 반환한다.
template<typename T> T LightHandle<T>::Get() const
{const auto data=m_scene.lock();if(!data || m_index>=data->template Lights<T>().size()){InvalidHandle("light");return {};}return data->template Lights<T>()[m_index];}
// 장면 수명과 광원 인덱스가 유효하면 값을 교체하고, 유효하지 않으면 진단 후 false를 반환한다.
template<typename T> bool LightHandle<T>::Set(const T& value)
{const auto data=m_scene.lock();if(!data || m_index>=data->template Lights<T>().size())return InvalidHandle("light");data->template Lights<T>()[m_index]=value;return true;}

// 방향광 배열 끝에 값을 추가하고 그 인덱스의 Handle을 반환한다. 할당 실패는 빈 Handle이다.
LightHandle<DirectionalLight> Scene::Add(const DirectionalLight& light)
{
    try {LightHandle<DirectionalLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<DirectionalLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<DirectionalLight>;

// 점광원을 별도 배열에 추가한다. 위치·범위 등의 렌더링 해석은 조명 준비 단계가 맡는다.
LightHandle<PointLight> Scene::Add(const PointLight& light)
{
    try {LightHandle<PointLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<PointLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<PointLight>;

// 스폿광원 값을 보관하고 타입이 맞는 Handle을 반환한다.
LightHandle<SpotLight> Scene::Add(const SpotLight& light)
{
    try {LightHandle<SpotLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<SpotLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<SpotLight>;

// 사각 면광원 값을 보관한다. 실제 셰이더 용량에 맞춘 활성 광원 선택은 렌더러가 수행한다.
LightHandle<RectAreaLight> Scene::Add(const RectAreaLight& light)
{
    try {LightHandle<RectAreaLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<RectAreaLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<RectAreaLight>;

// 원형 면광원 값을 보관한다. 추가 실패를 예외 대신 빈 Handle로 알린다.
LightHandle<DiscAreaLight> Scene::Add(const DiscAreaLight& light)
{
    try {LightHandle<DiscAreaLight> handle;handle.m_scene=m_lifetime;auto& lights=Lights<DiscAreaLight>();handle.m_index=static_cast<uint32_t>(lights.size());lights.push_back(light);return handle;}
    catch(const std::exception& error){std::fprintf(stderr,"dyf: light creation: %s\n",error.what());return {};}
}
template struct LightHandle<DiscAreaLight>;
}
