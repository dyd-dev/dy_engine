#include "RendererInternal.h"
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
	void DestroyBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready)
	{
		if(device != nullptr && buffer != nullptr) device->DestroyBuffer(buffer);
		buffer = nullptr;
		ready = false;
	}

	bool EnsureBuffer(RHI::IDevice* device, RHI::BufferHandle& buffer, bool& ready,
		uint32_t sizeBytes, uint32_t stride, RHI::BufferUsage usage)
	{
		if(device == nullptr) return false;
		if(buffer != nullptr && buffer->GetDesc().size == sizeBytes) return true;
		DestroyBuffer(device, buffer, ready);
		buffer = device->CreateBuffer({sizeBytes, stride, usage, RHI::ResourceState::CopyDestination});
		return buffer != nullptr;
	}
}

void Renderer::Impl::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
    mesh.shadowArena={};
    shadowArenaDirty=true;shadowArenaAttempted=false;shadowArenaStablePreparations=0;
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
        {&vertices.front().u,2*sizeof(float),sizeof(RendererVertex)}
    };
    shadowIndices.resize(indices.size());
    meshopt_generateShadowIndexBufferMulti(shadowIndices.data(),indices.data(),indices.size(),
        vertices.size(),streams,2);
    std::vector<uint32_t> dense(vertices.size(),UINT32_MAX);
    for(auto& index:shadowIndices)
    {
        auto& mapped=dense[index];
        if(mapped==UINT32_MAX)
        {
            mapped=static_cast<uint32_t>(shadowVertices.size());
            shadowVertices.emplace_back();
            // Preserve the existing compact vertex bits, triangle and corner order.
            std::memcpy(&shadowVertices.back().px,&vertices[index].px,3*sizeof(float));
            std::memcpy(&shadowVertices.back().u,&vertices[index].u,2*sizeof(float));
        }
        index=mapped;
    }
    return true;
}

RHI::BufferHandle Renderer::Impl::MainIndexBuffer(const SceneMeshState& mesh,uint32_t level,
    const MaterialDesc& material,const RendererDrawDesc* input) const
{
    if(level)return mesh.lods[level-1].buffer;
    const bool originalVertices=!input ||
        ((!input->vertexBuffer || input->vertexBuffer==mesh.vertexBuffer) &&
         (!input->previousVertexBuffer || input->previousVertexBuffer==mesh.vertexBuffer));
    return stockDrawInputs && material.alphaMode!=MaterialAlphaMode::Blend && originalVertices &&
        mesh.mainIndexBuffer && mesh.mainIndexReady ? mesh.mainIndexBuffer : mesh.indexBuffer;
}

// 공개 RHI가 업로드 바이트를 복사하므로 메시별 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::Impl::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false;
    const bool needShadow=stockDrawInputs && config.lighting.enabled && config.lighting.direct && config.lighting.shadows;
    const bool needMain=stockDrawInputs;
	while(m_meshes.size() > scene.Meshes().size())
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
		if(mesh.prepared && (!config.enableLod || mesh.lodsGenerated) && (!preparePerMeshShadow || (mesh.shadowIndexReady && mesh.shadowVertexReady)) &&
            (!needMain || mesh.mainIndexReady)) continue;
        const MeshData& source = *sourceMesh;
		mesh.prepared=false;
        mesh.bounds={};
        for(const auto& vertex:source.vertices) {
            const auto& p=vertex.position;
            if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {mesh.bounds={};break;}
            mesh.bounds.Include(p);
        }
		if(source.vertices.size() > UINT32_MAX / sizeof(RendererVertex) ||
			source.indices.size() > UINT32_MAX / sizeof(uint32_t))
		{
			return false;
		}
		std::vector<RendererVertex> vertices;
		vertices.reserve(source.vertices.size());
		for(const auto& vertex : source.vertices)
		{
			vertices.push_back({vertex.position.x, vertex.position.y, vertex.position.z,
				vertex.normal.x, vertex.normal.y, vertex.normal.z, vertex.uv.x, vertex.uv.y,
				vertex.tangent.x, vertex.tangent.y, vertex.tangent.z, vertex.tangent.w});
		}
		std::vector<uint32_t> sequentialIndices;
		if(source.indices.empty())
		{
			sequentialIndices.reserve(source.vertices.size());
			for(uint32_t index = 0; index < source.vertices.size(); ++index) sequentialIndices.push_back(index);
		}
		const auto& indices = source.indices.empty() ? sequentialIndices : source.indices;
		if(vertices.empty())
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
                    {&vertices.front().tx,4*sizeof(float),sizeof(RendererVertex)}
                };
                mainIndices.resize(indices.size());
                meshopt_generateShadowIndexBufferMulti(mainIndices.data(),indices.data(),indices.size(),
                    vertices.size(),streams,4);
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
		if(config.enableLod && !mesh.lodsGenerated)
		{
			if(source.lods.size()>3) return false;
			auto levels=source.lods.empty() ? BuildMeshLods(source) : source.lods;
			if(levels.size()>3) return false;
			uint32_t previousCount=static_cast<uint32_t>(indices.size());float previousError=0;
			for(const auto& level:levels)
			{
				if(level.indices.empty() || level.indices.size()%3 || level.indices.size()>=previousCount ||
					level.indices.size()>UINT32_MAX/sizeof(uint32_t) || !std::isfinite(level.error) || level.error<previousError)
					return false;
				for(uint32_t index:level.indices) if(index>=vertices.size()) return false;
				previousCount=static_cast<uint32_t>(level.indices.size());previousError=level.error;
			}
			std::vector<SceneMeshState::IndexLod> pendingLods;
			pendingLods.reserve(levels.size());
			for(auto& level:levels)
			{
				SceneMeshState::IndexLod lod;lod.count=static_cast<uint32_t>(level.indices.size());lod.error=level.error;
				lod.indices=std::move(level.indices);pendingLods.push_back(std::move(lod));
			}
			mesh.lods=std::move(pendingLods);
			// 더 단순화할 수 없는 메시도 완료된 결과로 보관한다.
			mesh.lodsGenerated=true;
		}
		const uint32_t vertexBytes = static_cast<uint32_t>(vertices.size() * sizeof(RendererVertex));
		const uint32_t indexBytes = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));
		if(!EnsureBuffer(device, mesh.vertexBuffer, mesh.vertexReady, vertexBytes,
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
		if(!commands)
		{
			commands = device->AcquireCommandList();
			if(commands) resources.Keep(commands);
		}
		if(!commands) return false;
		if(!mesh.vertexReady)
		{
			if(!RecordBufferUpload(device, *commands, mesh.vertexBuffer, vertices.data(), vertexBytes,
				RHI::ResourceState::VertexBuffer))
			{
				uploadFailed = true;
				break;
			}
			uploaded.push_back(&mesh.vertexReady);
		}
		if(!mesh.indexReady)
		{
			if(!RecordBufferUpload(device, *commands, mesh.indexBuffer, indices.data(), indexBytes,
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
			if(lod.ready) continue;
			const uint32_t bytes=lod.count*sizeof(uint32_t);
			if(lod.indices.size()!=lod.count || !EnsureBuffer(device,lod.buffer,lod.ready,bytes,
				sizeof(uint32_t),RHI::BufferUsage::Index) ||
				!RecordBufferUpload(device,*commands,lod.buffer,lod.indices.data(),bytes,RHI::ResourceState::IndexBuffer))
			{uploadFailed=true;break;}
			uploaded.push_back(&lod.ready);
		}
		if(uploadFailed) break;
	}

	if(!commands) return !needShadow || !UsesStockGeometry() || PrepareShadowArena(scene,device);
	const bool closed = commands->Close();
	const bool submitted = closed && device->Submit(&commands, 1);
	if(submitted)
	{
		// 실패한 제출은 준비 완료로 표시하지 않아 다음 프레임에서 다시 업로드한다.
		for(bool* ready : uploaded) *ready = true;
		for(auto& mesh : m_meshes)
		{
			bool lodsReady=true;
			for(auto& lod:mesh.lods)
			{
				lodsReady=lodsReady && lod.ready;
				if(lod.ready) std::vector<uint32_t>().swap(lod.indices);
			}
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
    // The first generation uses original geometry until this quiet preparation;
    // later changes retain per-mesh compact fallbacks until the arena rebuild.
    // A second unchanged PrepareGeometry call is required in either case.
    if(!shadowArenaStablePreparations++)return true;
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
                    vertex.tangent.x,vertex.tangent.y,vertex.tangent.z,vertex.tangent.w});
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

void Renderer::Impl::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return;
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
    if(shadowArenaVertices)device->DestroyBuffer(shadowArenaVertices);
    if(shadowArenaIndices)device->DestroyBuffer(shadowArenaIndices);
    shadowArenaVertices=shadowArenaIndices=nullptr;shadowArenaGeneration=0;
    shadowArenaDirty=true;shadowArenaAttempted=false;shadowArenaReported=false;shadowArenaStablePreparations=0;
	entityLods.clear();lodMeshSources.clear();lodScene.reset();
	lodDebugActive=wireframeActive=false;
}
}
