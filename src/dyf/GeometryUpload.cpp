#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "DrawCommands.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/IDevice.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace dyf
{
namespace
{
	// 소유한 버퍼를 장치로 반환하고 핸들/업로드 상태를 함께 초기화한다. 이후 코드가 과거 핸들을 준비된 자원으로 재사용하지 않게 한다.
	void DestroyBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer); // 장치와 버퍼가 있을 때만 해제하고 이후 핸들은 항상 비운다.
		buffer = nullptr;
		ready = false;
	}

	// 요청 크기와 같으면 기존 버퍼를 유지한다. 크기가 다르면 기존 버퍼를 해제하고 CopyDestination 버퍼를 만든다. 생성 실패 시 buffer는 nullptr, ready는 false로 남으며 이전 버퍼를 복구하지 않는다.
	bool EnsureBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready,
		uint32_t sizeBytes, uint32_t stride, RHI::BufferUsage usage)
	{
		if(device == nullptr) return false; // 버퍼 생성에 필요한 장치가 없으면 실패한다.
		if(buffer != nullptr && buffer->GetDesc().size == sizeBytes) return true; // 동일 크기의 기존 버퍼는 재생성하지 않는다.
		DestroyBuffer(device, buffer, ready);
		buffer = device->CreateBuffer({sizeBytes, stride, usage, RHI::ResourceState::CopyDestination});
		return buffer != nullptr;
	}
}

// 한 메시의 원본/LOD 버퍼, 인덱스 수, 경계, 준비 상태를 함께 정리한다. 이 함수 자체는 Scene의 CPU MeshData를 삭제하지 않는다.
void Renderer::Impl::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
    mesh.shadowArena={};
    shadowArenaDirty=true;shadowArenaAttempted=false;
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	DestroyBuffer(device, mesh.shadowVertexBuffer, mesh.shadowVertexReady);
	DestroyBuffer(device, mesh.shadowIndexBuffer, mesh.shadowIndexReady);
	DestroyBuffer(device, mesh.mainIndexBuffer, mesh.mainIndexReady);
    mesh.shadowIndexFormat=RHI::Format::R32_UINT;
	for(auto& lod:mesh.lods) DestroyBuffer(device,lod.buffer,lod.ready);
	mesh.lods.clear();mesh.lodsGenerated=false;
	mesh.indexCount = 0;
    mesh.bounds={};
	mesh.prepared = false;
}

bool Renderer::Impl::BuildCompactShadowGeometry(const std::vector<RendererVertex>& vertices,
    const std::vector<uint32_t>& indices,std::vector<CompactShadowVertex>& shadowVertices,
    std::vector<uint32_t>& shadowIndices)
{
    shadowVertices.clear();shadowIndices.clear();
    if(vertices.empty() || indices.empty() || indices.size()%3)return true;
    for(uint32_t index:indices)if(index>=vertices.size())return false;
    const meshopt_Stream streams[]={
        {&vertices.front().px,3*sizeof(float),sizeof(RendererVertex)},
        {&vertices.front().u,2*sizeof(float),sizeof(RendererVertex)},
        {&vertices.front().ca,sizeof(float),sizeof(RendererVertex)}
    };
    shadowIndices.resize(indices.size());
    meshopt_generateShadowIndexBufferMulti(shadowIndices.data(),indices.data(),indices.size(),
        vertices.size(),streams,std::size(streams));
    std::vector<uint32_t> dense(vertices.size(),UINT32_MAX);
    for(auto& index:shadowIndices)
    {
        auto& mapped=dense[index];
        if(mapped==UINT32_MAX)
        {
            mapped=static_cast<uint32_t>(shadowVertices.size());
            shadowVertices.emplace_back();
            // Preserve compact vertex bits and each triangle's corner order.
            std::memcpy(&shadowVertices.back().px,&vertices[index].px,3*sizeof(float));
            std::memcpy(&shadowVertices.back().u,&vertices[index].u,2*sizeof(float));
            shadowVertices.back().alpha=vertices[index].ca;
        }
        index=mapped;
    }
    // Depth-only stock shadows keep each triangle's corner bits while reusing transformed vertices.
    meshopt_optimizeVertexCache(shadowIndices.data(),shadowIndices.data(),shadowIndices.size(),shadowVertices.size());
    const size_t compactCount=meshopt_optimizeVertexFetch(shadowVertices.data(),shadowIndices.data(),shadowIndices.size(),
        shadowVertices.data(),shadowVertices.size(),sizeof(CompactShadowVertex));
    shadowVertices.resize(compactCount);
    return true;
}

RHI::BufferHandle Renderer::Impl::MainIndexBuffer(const SceneMeshState& mesh,uint32_t level,
    const MaterialDesc& material,const RendererDrawDesc* input) const
{
    if(level)return mesh.lods[level-1].buffer;
    const bool originalVertices=!input ||
        ((!input->vertexBuffer || input->vertexBuffer==mesh.vertexBuffer) &&
         (!input->previousVertexBuffer || input->previousVertexBuffer==mesh.vertexBuffer));
    return stockDrawInputs && !UsesBlendedRendering(material) && originalVertices &&
        mesh.mainIndexBuffer && mesh.mainIndexReady ? mesh.mainIndexBuffer : mesh.indexBuffer;
}

// Scene 크기에 캐시를 맞춰 변경 메시지만 준비하며, 공개 RHI가 업로드 바이트를 복사하므로 메시별 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::Impl::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false; // 지오메트리를 준비할 장치가 없으면 중단한다.
    const bool needShadow=stockDrawInputs && config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    const bool needMain=stockDrawInputs;
	while(m_meshes.size() > scene.Meshes().size()) // Scene에서 사라진 뒤쪽 메시 캐시를 하나씩 정리한다.
	{
		DestroyMeshState(device, m_meshes.back());
		m_meshes.pop_back();
	}
	m_meshes.resize(scene.Meshes().size());
    // Establish the source generation before deciding whether the initial
    // per-mesh compact uploads can be deferred for the shared arena.
    for(uint32_t meshIndex=0;meshIndex<scene.Meshes().size();++meshIndex)
    {
        auto& mesh=m_meshes[meshIndex];
        const auto& sourceMesh=scene.Meshes()[meshIndex];
        if(mesh.source.lock()!=sourceMesh)
        {
            DestroyMeshState(device,mesh);
            mesh.source=sourceMesh;
        }
    }
    const bool preferShadowArena=needShadow && UsesStockGeometry();
    // Once an arena exists, dynamic meshes keep the existing per-mesh fallback.
    // A failed initial arena attempt also enables that fallback on the next call.
    const bool deferPerMeshShadow=preferShadowArena && !shadowArenaGeneration && !shadowArenaAttempted;
    const bool preparePerMeshShadow=needShadow && !deferPerMeshShadow;

	RHI::ICommandList* commands = nullptr;
	// 획득한 명령 목록의 소유권을 범위로 묶어 조기 반환에서도 장치에 돌려준다.
	RHI::ResourceScope resources(*device);
	bool uploadFailed = false;
	std::vector<bool*> uploaded;
    uint32_t shadowUploads=0;
    uint64_t shadowUploadBytes=0;
    uint32_t shadowVertexUploads=0;
    uint64_t shadowVertexUploadBytes=0,shadowUniqueVertices=0;
    uint32_t mainUploads=0;
    uint64_t mainUploadBytes=0,mainSourceVertices=0,mainSharedVertices=0,mainRemappedCorners=0;
	for(uint32_t meshIndex = 0; meshIndex < scene.Meshes().size(); ++meshIndex)
	{
		SceneMeshState& mesh = m_meshes[meshIndex];
		const auto& sourceMesh = scene.Meshes()[meshIndex];
		// 원본·LOD·추가 주/그림자 인덱스의 업로드 제출 상태가 모두 준비된 메시만 재사용한다. prepared는 GPU 완료 확인값이 아니다.
		if(mesh.prepared && (!config.enableLod || mesh.lodsGenerated) && (!preparePerMeshShadow || (mesh.shadowIndexReady && mesh.shadowVertexReady)) &&
            (!needMain || mesh.mainIndexReady)) continue;
        const MeshData& source = *sourceMesh;
		mesh.prepared=false;
        mesh.bounds={};
        // 유한한 위치만 경계에 포함한다. 잘못된 위치가 발견되면 경계를 무효화하여 잘못된 경계 기반 제거에 의존하지 않게 한다.
        for(const auto& vertex:source.vertices) {
            const auto& p=vertex.position;
            if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {mesh.bounds={};break;} // 비유한 위치 하나라도 있으면 경계를 무효화하고 경계 계산을 멈춘다.
            mesh.bounds.Include(p);
        }
		if(source.vertices.size() > UINT32_MAX / sizeof(RendererVertex) || // 정점/인덱스 바이트 수가 32비트 크기를 넘으면 실패한다.
			source.indices.size() > UINT32_MAX / sizeof(uint32_t))
		{
			return false;
		}
		// 공개 Mesh 정점의 표면에 수직인 방향(법선)과 표면을 따라가는 방향(접선)을 렌더러 입력에 복사한다.
		// 위치·법선·UV·접선과 방향 부호를 셰이더 입력에 맞는 48바이트 RendererVertex로 변환한다.
		std::vector<RendererVertex> vertices;
		vertices.reserve(source.vertices.size());
		for(const auto& vertex : source.vertices)
		{
			vertices.push_back({vertex.position.x, vertex.position.y, vertex.position.z,
				vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
				vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w,
				vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w});
		}
		std::vector<uint32_t> sequentialIndices;
		if(source.indices.empty()) // 인덱스를 생략한 메시에는 0부터 연속 인덱스를 만들어 같은 indexed draw 경로를 사용한다.
		{
			sequentialIndices.reserve(source.vertices.size());
			for(uint32_t index = 0; index < source.vertices.size(); ++index) sequentialIndices.push_back(index);
		}
		const auto& indices = source.indices.empty() ? sequentialIndices : source.indices;
		if(vertices.empty()) // 빈 메시는 GPU 자원을 비우되 준비 처리된 빈 결과로 저장한다.
		{
			DestroyMeshState(device, mesh);
            mesh.source=sourceMesh;
			mesh.shadowIndexReady=true;
			mesh.shadowVertexReady=true;
			mesh.mainIndexReady=true;
			mesh.lodsGenerated=true;
			mesh.prepared = true;
			continue;
		}
        std::vector<uint32_t> shadowIndices;
        std::vector<uint16_t> shadowIndices16;
        std::vector<CompactShadowVertex> shadowVertices;
        uint32_t shadowIndexBytes=0,shadowIndexStride=sizeof(uint32_t);
        const void* shadowIndexData=nullptr;
        if(preparePerMeshShadow && (!mesh.shadowIndexReady || !mesh.shadowVertexReady))
        {
            // Source changes already invalidated the arena above. In particular,
            // an allocation-failure fallback must not restart its one-shot attempt.
            if(!BuildCompactShadowGeometry(vertices,indices,shadowVertices,shadowIndices))return false;
            if(shadowIndices.empty())mesh.shadowIndexReady=mesh.shadowVertexReady=true;
            else
            {
                if(*std::max_element(shadowIndices.begin(),shadowIndices.end())<=UINT16_MAX)
                {
                    shadowIndices16.assign(shadowIndices.begin(),shadowIndices.end());
                    shadowIndexStride=sizeof(uint16_t);
                    shadowIndexData=shadowIndices16.data();
                    mesh.shadowIndexFormat=RHI::Format::R16_UINT;
                }
                else
                {
                    shadowIndexData=shadowIndices.data();
                    mesh.shadowIndexFormat=RHI::Format::R32_UINT;
                }
                shadowIndexBytes=static_cast<uint32_t>(shadowIndices.size()*shadowIndexStride);
            }
        }
        std::vector<uint32_t> mainIndices;
        uint64_t sourceReferenced=0,sharedReferenced=0,remappedCorners=0;
        if(needMain && !mesh.mainIndexReady)
        {
            if(indices.size()%3==0)
            {
                for(uint32_t index:indices)if(index>=vertices.size())return false;
                // Every stock vertex attribute must be binary-equal. Original
                // buffers and triangle/corner order remain available to overrides.
                const meshopt_Stream streams[]={
                    {&vertices.front().px,3*sizeof(float),sizeof(RendererVertex)},
                    {&vertices.front().nx,3*sizeof(float),sizeof(RendererVertex)},
                    {&vertices.front().u,2*sizeof(float),sizeof(RendererVertex)},
                    {&vertices.front().tx,4*sizeof(float),sizeof(RendererVertex)},
                    {&vertices.front().cr,4*sizeof(float),sizeof(RendererVertex)}
                };
                mainIndices.resize(indices.size());
                meshopt_generateShadowIndexBufferMulti(mainIndices.data(),indices.data(),indices.size(),
                    vertices.size(),streams,std::size(streams));
                if(mainIndices==indices)mainIndices.clear();
                else
                {
                    std::vector<uint8_t> referenced(vertices.size(),0);
                    for(size_t i=0;i<indices.size();++i)
                    {
                        const uint32_t original=indices[i],shared=mainIndices[i];
                        if(!(referenced[original]&1u)) {++sourceReferenced;referenced[original]|=1u;}
                        if(!(referenced[shared]&2u)) {++sharedReferenced;referenced[shared]|=2u;}
                        remappedCorners+=original!=shared;
                    }
                    // Renaming a referenced vertex to an unused equivalent alone
                    // adds memory without creating any index reuse.
                    if(sharedReferenced==sourceReferenced)mainIndices.clear();
                }
            }
            if(mainIndices.empty())mesh.mainIndexReady=true;
        }
		if(config.enableLod && !mesh.lodsGenerated) // LOD가 켜져 있고 아직 생성하지 않은 메시만 LOD를 준비한다.
		{
			if(source.lods.size()>3) return false; // 입력 LOD가 최대 세 단계를 넘으면 거부한다.
			// 명시 LOD가 있으면 그 입력을 사용하고 없으면 CPU 단순화 결과를 구한다. 원본 이후 최대 세 단계이며 인덱스 감소와 오차의 비감소를 검사한다.
			auto levels=source.lods.empty() ? BuildMeshLods(source) : source.lods;
			if(levels.size()>3) return false; // 자동 생성 결과도 같은 최대 단계 수를 지켜야 한다.
			uint32_t previousCount=static_cast<uint32_t>(indices.size());float previousError=0;
			for(const auto& level:levels)
			{
				if(level.indices.empty() || level.indices.size()%3 || level.indices.size()>=previousCount || // 인덱스가 없거나 3의 배수가 아니거나 이전 단계보다 줄지 않으면 거부한다. 32비트 바이트 범위를 넘거나 오차가 비유한 값 또는 이전 단계보다 작아도 거부한다.
					level.indices.size()>UINT32_MAX/sizeof(uint32_t) || !std::isfinite(level.error) || level.error<previousError)
					return false;
				for(uint32_t index:level.indices) if(index>=vertices.size()) return false; // LOD 인덱스가 실제 정점 범위를 벗어나면 업로드하지 않는다.
				previousCount=static_cast<uint32_t>(level.indices.size());previousError=level.error;
			}
			// 검사가 끝난 LOD를 먼저 임시 배열에 모은 뒤 mesh에 옮긴다. 각 단계의 CPU 인덱스는 업로드 제출 성공 후 비운다.
			std::vector<SceneMeshState::IndexLod> pendingLods;
			pendingLods.reserve(levels.size());
			for(auto& level:levels)
			{
				SceneMeshState::IndexLod lod;lod.count=static_cast<uint32_t>(level.indices.size());lod.error=level.error;
				lod.indices=std::move(level.indices);pendingLods.push_back(std::move(lod));
			}
			mesh.lods=std::move(pendingLods);
			// 생성된 LOD가 없어도 lodsGenerated를 켜서, 더 줄일 수 없는 메시를 다음 호출에서 다시 단순화하지 않는다.
			mesh.lodsGenerated=true;
		}
		const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
		if(!EnsureBuffer(device, mesh.vertexBuffer, mesh.vertexReady, vertexBytes, // 정점 또는 원본 인덱스 버퍼를 확보하지 못하면 준비를 중단한다.
				sizeof(RendererVertex), RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage) ||
			!EnsureBuffer(device, mesh.indexBuffer, mesh.indexReady, indexBytes,
				sizeof(uint32_t), RHI::BufferUsage::Index))
		{
			return false;
		}
        if(!shadowIndices.empty() && !EnsureBuffer(device,mesh.shadowIndexBuffer,mesh.shadowIndexReady,
            shadowIndexBytes,shadowIndexStride,RHI::BufferUsage::Index))return false;
        if(!shadowVertices.empty() && !EnsureBuffer(device,mesh.shadowVertexBuffer,mesh.shadowVertexReady,
            static_cast<uint32_t>(shadowVertices.size()*sizeof(CompactShadowVertex)),sizeof(CompactShadowVertex),RHI::BufferUsage::Vertex))return false;
        if(!mainIndices.empty() && !EnsureBuffer(device,mesh.mainIndexBuffer,mesh.mainIndexReady,
            indexBytes,sizeof(uint32_t),RHI::BufferUsage::Index))return false;
		mesh.indexCount = static_cast<uint32_t>(indices.size());
		if(!commands) // 업로드할 메시를 처음 만났을 때만 공통 명령 목록을 획득한다.
		{
			commands = device->AcquireCommandList();
			if(commands) resources.Keep(commands); // 획득에 성공한 명령 목록만 범위 소유자에 등록한다.
		}
		if(!commands) return false; // 명령 목록을 얻지 못했으면 데이터를 기록할 수 없다.
		if(!mesh.vertexReady) // 아직 제출되지 않은 정점 버퍼만 업로드를 기록한다.
		{
			if(!RecordBufferUpload(device, *commands, mesh.vertexBuffer, vertices.data(), vertexBytes, // 정점 업로드 기록 실패를 표시하고 메시 준비 반복을 중단한다.
				RHI::ResourceState::VertexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.vertexReady);
		}
		if(!mesh.indexReady) // 아직 제출되지 않은 원본 인덱스 버퍼만 기록한다.
		{
			if(!RecordBufferUpload(device, *commands, mesh.indexBuffer, indices.data(), indexBytes, // 인덱스 업로드 기록 실패면 전체 준비 결과를 실패로 남긴다.
				RHI::ResourceState::IndexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.indexReady);
		}
        if(preparePerMeshShadow && mesh.shadowVertexBuffer && !mesh.shadowVertexReady)
        {
            const auto bytes=static_cast<uint32_t>(shadowVertices.size()*sizeof(CompactShadowVertex));
            if(!RecordBufferUpload(device,*commands,mesh.shadowVertexBuffer,shadowVertices.data(),bytes,
                RHI::ResourceState::VertexBuffer))
            {
                uploadFailed=true;
                break;
            }
            uploaded.push_back(&mesh.shadowVertexReady);
            ++shadowVertexUploads;shadowVertexUploadBytes+=bytes;shadowUniqueVertices+=shadowVertices.size();
        }
        if(preparePerMeshShadow && mesh.shadowIndexBuffer && !mesh.shadowIndexReady)
        {
            if(!RecordBufferUpload(device,*commands,mesh.shadowIndexBuffer,shadowIndexData,shadowIndexBytes,
                RHI::ResourceState::IndexBuffer))
            {
                uploadFailed=true;
                break;
            }
            uploaded.push_back(&mesh.shadowIndexReady);
            ++shadowUploads;
            shadowUploadBytes+=shadowIndexBytes;
        }
        if(needMain && mesh.mainIndexBuffer && !mesh.mainIndexReady)
        {
            if(!RecordBufferUpload(device,*commands,mesh.mainIndexBuffer,mainIndices.data(),indexBytes,
                RHI::ResourceState::IndexBuffer))
            {
                uploadFailed=true;
                break;
            }
            uploaded.push_back(&mesh.mainIndexReady);
            ++mainUploads;mainUploadBytes+=indexBytes;
            mainSourceVertices+=sourceReferenced;mainSharedVertices+=sharedReferenced;
            mainRemappedCorners+=remappedCorners;
        }
		for(auto& lod:mesh.lods)
		{
			if(lod.ready) continue; // 이미 제출한 LOD 버퍼는 다시 보내지 않는다.
			const uint32_t bytes=lod.count*sizeof(uint32_t);
			if(lod.indices.size()!=lod.count || !EnsureBuffer(device,lod.buffer,lod.ready,bytes, // LOD CPU 개수·GPU 버퍼 확보·업로드 기록 중 하나라도 실패하면 중단한다.
				sizeof(uint32_t),RHI::BufferUsage::Index) ||
				!RecordBufferUpload(device,*commands,lod.buffer,lod.indices.data(),bytes,RHI::ResourceState::IndexBuffer))
			{uploadFailed=true;break;}
			uploaded.push_back(&lod.ready);
		}
		if(uploadFailed) break; // LOD 준비에서 생긴 실패도 바깥 메시 반복까지 중단시킨다.
	}

	if(!commands) return !needShadow || !UsesStockGeometry() || PrepareShadowArena(scene,device);
	// 기록된 업로드는 같은 큐에 제출한다. 제출 성공과 전체 메시 준비 성공은 별도로 판정한다.
	const bool closed = commands->Close();
	const bool submitted = closed && device->Submit(&commands, 1);
	// 이번 제출이 성공한 버퍼만 ready로 확정하며 GPU 완료를 뜻하지 않는다. 제출 실패 시에는 ready를 새로 확정하지 않아 다음 프레임에 재업로드한다.
	if(submitted)
	{
		// 일부 메시 기록에 실패했다면 제출한 버퍼를 확정해도 전체 함수는 false를 반환한다.
		for(bool* ready : uploaded) *ready = true;
		for(auto& mesh : m_meshes)
		{
			bool lodsReady=true;
			for(auto& lod:mesh.lods)
			{
				lodsReady=lodsReady && lod.ready;
				if(lod.ready) std::vector<uint32_t>().swap(lod.indices); // LOD 업로드가 제출되었으면 재시도용 CPU 인덱스 저장소를 비운다.
			}
			// 필요한 원본·주/그림자 버퍼와 LOD의 업로드 제출이 성공했을 때만 prepared를 확정한다.
            // Original geometry is renderable while the optional first arena is
            // deferred; compact ready flags remain false until their own upload.
			if(mesh.vertexReady && mesh.indexReady && (!preparePerMeshShadow || (mesh.shadowIndexReady && mesh.shadowVertexReady)) &&
                (!needMain || mesh.mainIndexReady) && lodsReady)
                mesh.prepared = true;
		}
        if(deferPerMeshShadow && !uploadFailed)
            std::fprintf(stderr,"[Renderer] Initial shadow compact upload deferral: vertexBuffers=%u vertexBytes=%llu indexBuffers=%u indexBytes=%llu; original-buffer fallback until arena commit.\n",
                shadowVertexUploads,static_cast<unsigned long long>(shadowVertexUploadBytes),
                shadowUploads,static_cast<unsigned long long>(shadowUploadBytes));
        if(shadowUploads)
            std::fprintf(stderr,"[Renderer] Shadow index buffers uploaded: count=%u bytes=%llu\n",
                shadowUploads,static_cast<unsigned long long>(shadowUploadBytes));
        if(shadowVertexUploads)
            std::fprintf(stderr,"[Renderer] Compact shadow vertex buffers uploaded: count=%u bytes=%llu uniqueVertices=%llu stride=%u\n",
                shadowVertexUploads,static_cast<unsigned long long>(shadowVertexUploadBytes),
                static_cast<unsigned long long>(shadowUniqueVertices),static_cast<unsigned>(sizeof(CompactShadowVertex)));
        if(mainUploads)
            std::fprintf(stderr,"[Renderer] Main shared index buffers uploaded: count=%u bytes=%llu sourceUnique=%llu sharedUnique=%llu remappedCorners=%llu\n",
                mainUploads,static_cast<unsigned long long>(mainUploadBytes),static_cast<unsigned long long>(mainSourceVertices),
                static_cast<unsigned long long>(mainSharedVertices),static_cast<unsigned long long>(mainRemappedCorners));
	}
	if(!submitted || uploadFailed)return false;
    return !needShadow || !UsesStockGeometry() || PrepareShadowArena(scene,device);
}

bool Renderer::Impl::PrepareShadowArena(const Scene& scene,RHI::IDevice* device)
{
    if(!shadowArenaDirty || shadowArenaAttempted)return true;
    // Registered mesh sources are immutable and all original uploads above
    // have been submitted. Build this source generation now; an unchanged
    // preparation count is not evidence of future mesh stability.
    // Optional allocation is tried once for each source/compact-geometry change.
    // An unchanged scene must not repeat a failed large allocation every frame.
    shadowArenaAttempted=true;
    if(shadowArenaGeneration==std::numeric_limits<uint64_t>::max())return true;
    struct Pending
    {
        RHI::IDevice& device;
        RHI::BufferHandle vertices=nullptr,indices=nullptr;
        RHI::ICommandList* commands=nullptr;
        ~Pending()
        {
            if(commands)device.DestroyCommandList(commands);
            if(vertices)device.DestroyBuffer(vertices);
            if(indices)device.DestroyBuffer(indices);
        }
    } pending{*device};
    std::vector<CompactShadowVertex> vertices;
    std::vector<uint8_t> indices;
    std::vector<ShadowArenaRange> ranges;
    uint32_t meshCount=0;
    const uint64_t generation=shadowArenaGeneration+1;
    try
    {
        ranges.resize(m_meshes.size());
        std::vector<RendererVertex> sourceVertices;
        std::vector<CompactShadowVertex> compactVertices;
        std::vector<uint32_t> sourceIndices,compactIndices;
        for(size_t index=0;index<m_meshes.size();++index)
        {
            const auto& mesh=m_meshes[index];
            if(!mesh.prepared || !mesh.vertexReady || !mesh.indexReady || !mesh.indexCount)continue;
            const auto& source=*scene.Meshes()[index];
            sourceVertices.clear();sourceVertices.reserve(source.vertices.size());
            for(const auto& vertex:source.vertices)
                sourceVertices.push_back({vertex.position.x,vertex.position.y,vertex.position.z,
                    vertex.normal.x,vertex.normal.y,vertex.normal.z,vertex.uv.x,vertex.uv.y,
                    vertex.tangent.x,vertex.tangent.y,vertex.tangent.z,vertex.tangent.w,
                    vertex.color.x,vertex.color.y,vertex.color.z,vertex.color.w});
            sourceIndices.clear();
            if(source.indices.empty())
            {
                sourceIndices.reserve(source.vertices.size());
                for(uint32_t vertex=0;vertex<source.vertices.size();++vertex)sourceIndices.push_back(vertex);
            }
            const auto& inputIndices=source.indices.empty()?sourceIndices:source.indices;
            if(!BuildCompactShadowGeometry(sourceVertices,inputIndices,compactVertices,compactIndices))return false;
            if(compactIndices.empty())continue;
            if(compactIndices.size()!=mesh.indexCount)return false;
            const uint32_t maximum=*std::max_element(compactIndices.begin(),compactIndices.end());
            const auto format=maximum<=UINT16_MAX?RHI::Format::R16_UINT:RHI::Format::R32_UINT;
            const uint32_t indexSize=format==RHI::Format::R16_UINT?2u:4u;
            const uint64_t indexOffset=(uint64_t(indices.size())+3u)&~uint64_t{3};
            const uint64_t indexBytes=uint64_t(compactIndices.size())*indexSize;
            const uint64_t vertexCount=uint64_t(vertices.size())+compactVertices.size();
            if(vertexCount>UINT32_MAX/sizeof(CompactShadowVertex) || vertexCount>vertices.max_size() ||
                vertices.size()>INT32_MAX || indexOffset>UINT32_MAX || indexBytes>UINT32_MAX-indexOffset ||
                indexOffset+indexBytes>indices.max_size())
                return true; // Keep existing arena/per-mesh/original-buffer fallbacks.
            auto& range=ranges[index];
            range.generation=generation;range.baseVertex=static_cast<uint32_t>(vertices.size());
            range.indexByteOffset=static_cast<uint32_t>(indexOffset);range.indexFormat=format;
            range.sortSafeInputs=true;
            for(const auto& vertex:compactVertices)
                for(float value:{vertex.px,vertex.py,vertex.pz,vertex.u,vertex.v})
                    if(!std::isfinite(value) || std::abs(value)>1e10f)range.sortSafeInputs=false;
            const size_t vertexOffset=vertices.size();
            vertices.resize(static_cast<size_t>(vertexCount));
            std::memcpy(vertices.data()+vertexOffset,compactVertices.data(),compactVertices.size()*sizeof(CompactShadowVertex));
            indices.resize(static_cast<size_t>(indexOffset+indexBytes));
            if(indexSize==2)
            {
                for(size_t corner=0;corner<compactIndices.size();++corner)
                {
                    const uint16_t value=static_cast<uint16_t>(compactIndices[corner]);
                    std::memcpy(indices.data()+indexOffset+corner*2,&value,sizeof(value));
                }
            }
            else std::memcpy(indices.data()+indexOffset,compactIndices.data(),static_cast<size_t>(indexBytes));
            ++meshCount;
        }
        if(vertices.empty() || indices.empty())
        {
            // No surviving source needs the old immutable arena.
            if(shadowArenaVertices)device->DestroyBuffer(shadowArenaVertices);
            if(shadowArenaIndices)device->DestroyBuffer(shadowArenaIndices);
            shadowArenaVertices=shadowArenaIndices=nullptr;shadowArenaGeneration=0;
            for(auto& mesh:m_meshes)mesh.shadowArena={};
            shadowArenaDirty=false;
            return true;
        }
        pending.vertices=device->CreateBuffer({static_cast<uint32_t>(vertices.size()*sizeof(CompactShadowVertex)),
            sizeof(CompactShadowVertex),RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        if(!pending.vertices)return true;
        // Index buffers are untyped bytes; each aligned range retains its R16/R32 format.
        pending.indices=device->CreateBuffer({static_cast<uint32_t>(indices.size()),sizeof(uint32_t),
            RHI::BufferUsage::Index,RHI::ResourceState::CopyDestination});
        if(!pending.indices)return true;
    }
    catch(const std::bad_alloc&)
    {
        return true; // No GPU commands have been opened or modified yet.
    }
    pending.commands=device->AcquireCommandList();
    if(!pending.commands)return false;
    if(!RecordBufferUpload(device,*pending.commands,pending.vertices,vertices.data(),
        static_cast<uint32_t>(vertices.size()*sizeof(CompactShadowVertex)),RHI::ResourceState::VertexBuffer) ||
        !RecordBufferUpload(device,*pending.commands,pending.indices,indices.data(),static_cast<uint32_t>(indices.size()),
        RHI::ResourceState::IndexBuffer) || !pending.commands->Close() || !device->Submit(&pending.commands,1))
        return false;

    // Commit only after the full immutable arena submission succeeded. Other
    // unchanged meshes can continue using the previous arena if preparation fails.
    auto* oldVertices=shadowArenaVertices;auto* oldIndices=shadowArenaIndices;
    shadowArenaVertices=std::exchange(pending.vertices,nullptr);
    shadowArenaIndices=std::exchange(pending.indices,nullptr);
    shadowArenaGeneration=generation;shadowArenaDirty=false;shadowArenaReported=false;
    uint64_t retiredBytes=0;
    for(size_t index=0;index<m_meshes.size();++index)
    {
        auto& mesh=m_meshes[index];mesh.shadowArena=ranges[index];
        if(mesh.shadowArena.generation!=generation)continue;
        if(mesh.shadowVertexBuffer)retiredBytes+=mesh.shadowVertexBuffer->GetDesc().size;
        if(mesh.shadowIndexBuffer)retiredBytes+=mesh.shadowIndexBuffer->GetDesc().size;
        DestroyBuffer(device,mesh.shadowVertexBuffer,mesh.shadowVertexReady);
        DestroyBuffer(device,mesh.shadowIndexBuffer,mesh.shadowIndexReady);
        // Ready now denotes the committed arena range; avoid rebuilding per-mesh copies.
        mesh.shadowVertexReady=mesh.shadowIndexReady=true;
    }
    if(oldVertices)device->DestroyBuffer(oldVertices);
    if(oldIndices)device->DestroyBuffer(oldIndices);
    shadowAtlasCache.valid=false;shadowAtlasCache.pending=false;
    std::fprintf(stderr,"[Renderer] Shared shadow arena uploaded: generation=%llu meshes=%u vertexBytes=%llu indexBytes=%llu retiredCompactBytes=%llu\n",
        static_cast<unsigned long long>(generation),meshCount,
        static_cast<unsigned long long>(vertices.size()*sizeof(CompactShadowVertex)),
        static_cast<unsigned long long>(indices.size()),static_cast<unsigned long long>(retiredBytes));
    return true;
}

// 모든 메시 GPU 캐시와 객체별 LOD 이력을 비운다. 다음 사용 시 원본 Scene에서 다시 준비할 수 있게 진단 상태도 초기화한다.
void Renderer::Impl::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return; // 장치가 없으면 GPU 캐시 해제 루틴을 호출하지 않는다.
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
    if(shadowArenaVertices)device->DestroyBuffer(shadowArenaVertices);
    if(shadowArenaIndices)device->DestroyBuffer(shadowArenaIndices);
    shadowArenaVertices=shadowArenaIndices=nullptr;shadowArenaGeneration=0;
    shadowArenaDirty=true;shadowArenaAttempted=false;shadowArenaReported=false;
	entityLods.clear();lodMeshSources.clear();lodScene.reset();
	lodDebugActive=wireframeActive=false;
}
}
