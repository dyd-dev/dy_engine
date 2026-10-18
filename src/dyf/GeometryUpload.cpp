#include "RendererInternal.h"
#include "DrawCommands.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/IDevice.h"
#include <cmath>
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
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	for(auto& lod:mesh.lods) DestroyBuffer(device,lod.buffer,lod.ready);
	mesh.lods.clear();mesh.lodsGenerated=false;
	mesh.indexCount = 0;
    mesh.bounds={};
	mesh.prepared = false;
}

// 공개 RHI가 업로드 바이트를 복사하므로 메시별 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::Impl::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
{
	if(device == nullptr) return false;
	while(m_meshes.size() > scene.Meshes().size())
	{
		DestroyMeshState(device, m_meshes.back());
		m_meshes.pop_back();
	}
	m_meshes.resize(scene.Meshes().size());

	RHI::ICommandList* commands = nullptr;
	RHI::ResourceScope resources(*device);
	bool uploadFailed = false;
	std::vector<bool*> uploaded;
	for(uint32_t meshIndex = 0; meshIndex < scene.Meshes().size(); ++meshIndex)
	{
		SceneMeshState& mesh = m_meshes[meshIndex];
		const auto& sourceMesh = scene.Meshes()[meshIndex];
		if(mesh.source.lock() != sourceMesh)
		{
			DestroyMeshState(device, mesh);
			mesh.source = sourceMesh;
		}
		if(mesh.prepared && (!config.enableLod || mesh.lodsGenerated)) continue;
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
			mesh.prepared = true;
			continue;
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

	if(!commands) return true;
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
			if(mesh.vertexReady && mesh.indexReady && lodsReady) mesh.prepared = true;
		}
	}
	return submitted && !uploadFailed;
}

void Renderer::Impl::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return;
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
	entityLods.clear();lodMeshSources.clear();lodScene.reset();
	lodDebugActive=wireframeActive=false;
}
}
