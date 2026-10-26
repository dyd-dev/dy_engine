#include "dyf/Mesh.h"
#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>

namespace dyf
{
// 메시 원본을 바꾸지 않고 최대 세 단계의 추가 인덱스 배열을 만든다. 생성 불가한 입력은 빈 결과로 반환하여 원본 렌더링을 유지한다.
std::vector<MeshLod> BuildMeshLods(const MeshData& mesh,uint32_t maxLevels)
{
    std::vector<MeshLod> result;
    const auto vertexCount=mesh.vertices.size();
    if(!maxLevels || vertexCount<3 || vertexCount>UINT32_MAX) return result; // 단계 요청이 없거나 정점 수가 삼각형/32비트 인덱스 범위를 만족하지 않으면 축약하지 않는다.
    std::vector<uint32_t> sequential;
    if(mesh.indices.empty()) // 비인덱스 메시에는 0,1,2… 순차 인덱스를 임시로 만들어 같은 축약 경로를 사용한다.
    {
        sequential.resize(vertexCount);
        for(uint32_t i=0;i<vertexCount;++i) sequential[i]=i;
    }
    const auto& original=mesh.indices.empty() ? sequential : mesh.indices;
    // A nonempty triangle list cannot shrink below one triangle. Every larger
    // valid list may attempt simplification under the same error policy.
    if(original.size()<=3 || original.size()%3 || original.size()>UINT32_MAX/sizeof(uint32_t)) return result;
    for(uint32_t index:original) if(index>=vertexCount) return result; // 범위 밖 정점 인덱스가 하나라도 있으면 외부 축약 함수에 잘못된 자료를 넘기지 않는다.
    for(const auto& v:mesh.vertices)
        for(float value:{v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y,
            v.color.x,v.color.y,v.color.z,v.color.w})
            if(!std::isfinite(value)) return result; // 위치·법선·UV·정점 색에 NaN·무한대가 있으면 축약 오차를 계산할 수 없어 원본 메시를 유지한다.
    // FBX triangle corners may be separate indices even when their attributes
    // are identical. Restore that connectivity only for simplification. Compare
    // actual components, never float3/Vertex padding or position alone.
    const auto& first=mesh.vertices.front();
    // tangent에는 표면을 따라가는 방향(접선)과 방향 부호가 들어 있다.
    // 위치만 같은 정점을 합치면 UV·법선 경계가 사라지므로 위치·법선·UV·tangent 네 성분·색이 모두 같은 정점만 축약용으로 연결한다.
    const meshopt_Stream streams[]={
        {&first.position.x,3*sizeof(float),sizeof(Vertex)},
        {&first.normal.x,3*sizeof(float),sizeof(Vertex)},
        {&first.uv.x,2*sizeof(float),sizeof(Vertex)},
        {&first.tangent.x,4*sizeof(float),sizeof(Vertex)},
        {&first.color.x,4*sizeof(float),sizeof(Vertex)}
    };
    std::vector<uint32_t> remap(vertexCount);
    const size_t compactCount=meshopt_generateVertexRemapMulti(remap.data(),original.data(),original.size(),
        vertexCount,streams,std::size(streams));
    if(compactCount<3) return result; // 중복 정점을 합친 뒤 삼각형을 만들 정점이 남지 않으면 결과를 만들지 않는다.
    // 압축 정점 번호별로 원본 정점 하나를 기억한다. 최종 LOD는 이 대표 번호를 통해 원래 정점 버퍼를 그대로 참조한다.
    std::vector<uint32_t> representatives(compactCount,UINT32_MAX);
    std::vector<std::array<float,3>> positions(compactCount);
    std::vector<std::array<float,9>> attributes(compactCount);
    for(uint32_t index:original)
    {
        const auto compact=remap[index];
        if(representatives[compact]!=UINT32_MAX) continue; // 이미 채운 압축 정점은 반복해 속성을 복사하지 않는다.
        representatives[compact]=index;
        const auto& v=mesh.vertices[index];
        positions[compact]={v.position.x,v.position.y,v.position.z};
        attributes[compact]={v.normal.x,v.normal.y,v.normal.z,v.uv.x,v.uv.y,
            v.color.x,v.color.y,v.color.z,v.color.w};
    }
    std::vector<uint32_t> current(original.size());
    // 임시 인덱스를 압축 정점 번호로 바꾼다. 이 번호는 축약 내부에서만 사용하고 반환 시 원본 번호로 복구한다.
    meshopt_remapIndexBuffer(current.data(),original.data(),original.size(),remap.data());
    std::vector<uint32_t>().swap(remap);
    const float scale=meshopt_simplifyScale(positions.front().data(),compactCount,sizeof(positions.front()));
    if(!std::isfinite(scale) || scale<=0) return result; // 메시 크기 기반 오차 환산값이 유한한 양수가 아니면 축약 단계를 만들지 않는다.
    // 법선 3성분·UV 2성분·정점 색 4성분에 오차 가중치를 주고 엄격한 위상 제약과 열린 경계 고정으로 음영·UV·색 경계 및 재질 사이의 틈을 보호한다.
    const float weights[]={0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f};
    result.reserve(std::min(maxLevels,3u));
    float cumulativeError=0;
    for(uint32_t level=0;level<std::min(maxLevels,3u);++level)
    {
        // Keep the requested half/quarter/eighth targets and triangle alignment;
        // the minimum is one valid triangle, not an estimated useful mesh size.
        const size_t target=std::max<size_t>(3,(original.size()>>(level+1))/3*3);
        if(target>=current.size()) break; // 목표가 현재보다 작지 않으면 더 축약할 의미가 없어 반복을 끝낸다.
        std::vector<uint32_t> reduced(current.size());
        float error=0;
        const size_t count=meshopt_simplifyWithAttributes(reduced.data(),current.data(),current.size(),
            positions.front().data(),compactCount,sizeof(positions.front()),attributes.front().data(),sizeof(attributes.front()),weights,9,nullptr,
            target,0.04f,meshopt_SimplifyLockBorder,&error);
        // Accept actual index-count progress; a guessed minimum percentage of
        // saved triangles is independent of the configured visual error limit.
        if(!count || count%3 || count>=current.size() || !std::isfinite(error) || error<0) break;
        reduced.resize(count);
        // 법선·UV·정점 색 가중치가 섞인 단계별 오차를 메시 단위로 환산·누적해 화면 픽셀 기반 LOD 선택에 쓰며, 엄밀한 기하 거리 상한으로 해석하지 않는다.
        cumulativeError+=error*scale;
        if(!std::isfinite(cumulativeError)) break; // 누적 오차가 수치 범위를 벗어나면 이 단계를 반환 목록에 넣지 않는다.
        // 축약된 삼각형 순서를 정점 캐시 재사용에 유리하게 정리한 후 원본 정점 번호로 변환한다.
        meshopt_optimizeVertexCache(reduced.data(),reduced.data(),count,compactCount);
        MeshLod lod;lod.indices.resize(count);
        lod.error=cumulativeError;
        for(size_t i=0;i<count;++i) lod.indices[i]=representatives[reduced[i]];
        result.push_back(std::move(lod));
        // 다음 단계는 방금 축약한 목록을 입력으로 삼는다. 누적 오차가 필요한 이유도 이 연쇄 축약 때문이다.
        current=std::move(reduced);
    }
    return result;
}

// 아직 엔티티별 선택 자료가 없는 인덱스는 안전한 기본값인 원본 단계 0을 돌려준다.
uint32_t Renderer::Impl::EntityLod(uint32_t entity) const
{
    return entity<entityLods.size() ? entityLods[entity] : 0;
}

// 디버그 표시용으로 원본·세 축약 단계에 서로 다른 색을 지정한다. 범위 밖 단계는 마지막 색으로 제한한다.
Math::float4 Renderer::Impl::LodColor(uint32_t level)
{
    constexpr float colors[4][3]={{0.1f,0.8f,0.25f},{0.95f,0.8f,0.08f},{1.0f,0.3f,0.04f},{0.6f,0.15f,1.0f}};
    const auto& c=colors[std::min(level,3u)];
    return {c[0],c[1],c[2],1};
}

// 장면과 메시가 이전 프레임과 이어지는지 확인하고, 월드 오차를 화면 픽셀 오차로 환산해 허용값을 넘지 않는 가장 큰 LOD 번호를 고른다.
void Renderer::Impl::SelectLods(const Scene& scene,const Camera& camera,uint32_t width,uint32_t height,
    const std::vector<RendererDrawDesc>* draws)
{
    // 주소만 비교하지 않고 공유 소유권의 정체성을 비교한다. 같은 메모리 주소에 새 장면이 생겨도 과거 LOD 이력이 이어지지 않는다.
    const std::weak_ptr<Scene> current=scene.m_lifetime;
    const bool same=!lodScene.expired() && !lodScene.owner_before(current) && !current.owner_before(lodScene);
    if(!same) {entityLods.clear();lodMeshSources.clear();} // 장면 소유자가 달라졌거나 사라졌으면 엔티티별 선택과 메시 원본 이력을 비운다.
    lodScene=current;
    entityLods.resize(scene.GetEntityCount(),0);
    lodMeshSources.resize(scene.GetEntityCount());
    // 기본 기하 입력이고 환경 캡처 중이 아닐 때만 LOD를 적용한다. 와이어프레임은 고급 렌더링 경로도 필요하다.
    const bool active=config.enableLod && stockDrawInputs && !m_environmentCapture;
    wireframeActive=config.debugWireframe && UsesAdvancedRendering() && stockDrawInputs && !m_environmentCapture;
    lodDebugActive=(config.debugLodColor || wireframeActive) && stockDrawInputs && !m_environmentCapture;
    if(!active) // 사용 조건이 맞지 않으면 모든 엔티티를 원본 단계로 돌리고 종료한다.
    {
        std::fill(entityLods.begin(),entityLods.end(),uint8_t{0});
        return;
    }
    // 가로·세로 중 더 엄격한 투영 배율을 픽셀 단위로 사용한다. 원근 투영은 거리로 나누고 직교 투영은 거리와 무관하다.
    const float projection=std::max(std::abs(camera.projection.m[0])*width,std::abs(camera.projection.m[5])*height)*0.5f;
    const bool perspective=std::abs(camera.projection.m[15])<1e-6f;
    const bool gpu=UsesGpuVisibility();
    for(uint32_t i=0;i<scene.GetEntityCount();++i)
    {
        const auto entity=static_cast<EntityID>(i);
        const auto material=scene.GetEntityMaterial(entity);
        if(gpu && IsValid(material) && ToIndex(material)<scene.Materials().size() && // GPU 가시성 대상인 비블렌드 객체는 GPU가 LOD를 선택하므로 CPU는 중복 계산하지 않는다.
            !UsesBlendedRendering(scene.Materials()[ToIndex(material)]))
        {
            entityLods[i]=0;continue;
        }
        const auto meshId=scene.GetEntityMesh(entity);
        if(!IsValid(meshId) || ToIndex(meshId)>=m_meshes.size()) {entityLods[i]=0;continue;} // 잘못된 메시 ID는 원본 단계 표시만 남기고 계산을 건너뛴다.
        const auto& mesh=m_meshes[ToIndex(meshId)];
        const auto source=mesh.source.lock();
        // LOD를 낮추는 조건과 높이는 조건에 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)은 이전 선택을 기준으로 삼는다.
        // 같은 엔티티 번호에 다른 메시가 연결되면 그 메시의 이전 선택으로 볼 수 없으므로 기준을 원본 단계 0으로 다시 잡는다.
        uint32_t previous=lodMeshSources[i].lock()==source ? entityLods[i] : 0;
        lodMeshSources[i]=source;
        entityLods[i]=0;
        if(draws && (*draws)[i].vertexBuffer) continue; // 외부 변형 정점에는 축약 오차가 보장되지 않아 원본을 유지한다.
        if(mesh.lods.empty() || !mesh.bounds.valid || !std::isfinite(projection)) continue; // 축약 자료·경계·유한한 투영값 중 하나라도 없으면 보수적으로 원본을 그린다.
        const auto& world=scene.GetTransform(entity);
        // ||A||2 <= sqrt(||A||1 * ||A||infinity), also conservative for shear.
        // 월드 변환의 3×3 부분에서 각 행과 열의 성분 절댓값을 더하고, 가장 큰 행합과 열합을 곱해 제곱근을 구한다.
        // 어떤 방향의 확대도 이 값보다 크지 않도록 상한을 잡아, 비균일 확대나 전단이 있는 물체의 LOD 오차를 실제보다 작게 계산하지 않는다.
        float columnMax=0,rowMax=0;
        for(uint32_t axis=0;axis<3;++axis)
        {
            float column=0,row=0;
            for(uint32_t k=0;k<3;++k) {column+=std::abs(world.m[axis*4+k]);row+=std::abs(world.m[k*4+axis]);}
            columnMax=std::max(columnMax,column);rowMax=std::max(rowMax,row);
        }
        const float scale=std::sqrt(columnMax*rowMax);
        if(!std::isfinite(scale) || scale<=0) continue; // 유효한 양수 확대율을 얻지 못하면 원본 단계에서 멈춘다.
        // 로컬 경계 상자를 감싸는 구를 월드로 옮긴다. 원근 오차는 중심 거리 대신 구의 가장 가까운 깊이 depth-radius로 계산한다.
        const auto center=Math::TransformPoint(world,(mesh.bounds.min+mesh.bounds.max)*0.5f);
        const float radius=std::sqrt(Math::LengthSquared((mesh.bounds.max-mesh.bounds.min)*0.5f))*scale;
        const float depth=-Math::TransformPoint(camera.view,center).z;
        if(perspective && depth<=radius+0.001f) continue; // 카메라가 경계 구 안/근처에 있으면 가까운 면의 오차를 안전하게 환산할 수 없어 축약하지 않는다.
        const float pixelsPerUnit=projection*(perspective ? 1.0f/(depth-radius) : 1.0f);
        const float screenSize=2.0f*radius*pixelsPerUnit;
        if(!std::isfinite(pixelsPerUnit) || pixelsPerUnit<0 || !std::isfinite(screenSize)) continue; // 픽셀 환산 배율·화면 지름이 유효하지 않으면 기존 0 선택을 유지한다.
        previous=std::min<uint32_t>(previous,mesh.lods.size());
        uint32_t selected=0;
        for(uint32_t level=1;level<=mesh.lods.size();++level)
        {
            const auto& lod=mesh.lods[level-1];
            // 더 거친 단계로 갈 때는 허용값을 줄이고 이전보다 세밀한 쪽에서는 늘린다. 같은 경계에서 LOD가 매 프레임 왕복하는 것을 줄이는 히스테리시스다.
            const float transition=level>previous ? 1-config.lodHysteresis : 1+config.lodHysteresis;
            const float limit=config.lodPixelError*transition;
            const float screenLimit=config.lodScreenSize*std::min(width,height)*transition/(1u<<(level-1));
            const float projected=lod.error*scale*pixelsPerUnit;
            if(!lod.ready || !lod.buffer || !std::isfinite(projected) || projected>limit || screenSize>screenLimit) break; // 화면 크기와 오차 기준을 모두 만족하는 축약 단계만 허용한다.
            selected=level;
        }
        // 모든 조건을 통과한 가장 큰 단계 번호를 이번 엔티티 선택으로 보관한다. 실제 인덱스 버퍼 선택은 후속 드로 경로가 수행한다.
        entityLods[i]=static_cast<uint8_t>(selected);
    }
}
}
