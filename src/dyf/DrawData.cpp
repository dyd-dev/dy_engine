#include "dyf/Renderer.h"
#include "dyf/Camera.h"
#include "dyf/Scene.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

#include "ShaderLayout.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ResourceSet.h"
#include "dyf/RHI/ResourceScope.h"
#include "dyf/RHI/Texture.h"

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

	bool RecordBufferUpload(RHI::IDevice* device, RHI::ICommandList& commands,
		RHI::BufferHandle buffer, const void* data, uint32_t sizeBytes, RHI::ResourceState useState)
	{
		if(device == nullptr || buffer == nullptr || data == nullptr || sizeBytes == 0) return false;
		if(!device->UpdateBuffer(commands, buffer, 0, data, sizeBytes)) return false;
		const RHI::ResourceBarrierDesc ready = {
			buffer, nullptr, RHI::ResourceState::CopyDestination, useState, {}
		};
		commands.ResourceBarrier(&ready, 1);
		return true;
	}

	DrawConstants MakeDrawConstants(const Math::float4x4& viewProjection,
		const MaterialDesc& material, const Transform& transform, uint32_t textureFlags)
	{
		DrawConstants constants = {};
		constants.viewProjectionMatrix = viewProjection;
		constants.modelMatrix = transform.worldMatrix;
		constants.textureFlags = textureFlags;
		constants.emissiveColor = {material.emissiveColor.x, material.emissiveColor.y, material.emissiveColor.z, 0};
		constants.baseColor = material.baseColor;
		constants.materialParams = {material.metallicFactor, material.roughnessFactor,
			material.normalScale, material.occlusionStrength};
		return constants;
	}
}

// CPU bytes are captured by UpdateBuffer. Only GPU copies touch these persistent
// buffers; upload and draw submissions use the same queue, so no CPU wait is needed.
bool Renderer::UpdateInstanceBuffer(const Scene& scene,RHI::ICommandList& commands)
{
    pendingInstanceScene=scene.m_lifetime;
    pendingInstanceRevision=scene.m_transformRevision;
    pendingInstanceCount=scene.GetEntityCount();
    if(!UsesInstanceStream() || !pendingInstanceCount) {
        instanceBuffer=nullptr;instanceReady=false;return true;
    }
    if(pendingInstanceCount>UINT32_MAX/sizeof(Math::float4x4))return false;
    const auto bytes=static_cast<uint32_t>(pendingInstanceCount*sizeof(Math::float4x4));
    if(!instanceBuffer || instanceBuffer->GetDesc().size!=bytes) {
        instanceBuffer=device->CreateBuffer({bytes,sizeof(Math::float4x4),RHI::BufferUsage::Vertex,RHI::ResourceState::CopyDestination});
        instanceReady=false;
    }
    if(!instanceBuffer)return false;
    const bool sameScene=!instanceScene.expired() &&
        !instanceScene.owner_before(pendingInstanceScene) && !pendingInstanceScene.owner_before(instanceScene);
    bool full=!instanceReady || !sameScene || instanceCount!=pendingInstanceCount;
    if(!full && instanceRevision==pendingInstanceRevision)return true;

    instanceScratch.resize(pendingInstanceCount);
    instanceRanges.clear();
    if(!full) {
        for(uint32_t i=0;i<pendingInstanceCount;++i) {
            if(scene.m_entityTransformRevisions[i]<=instanceRevision)continue;
            if(!instanceRanges.empty() && instanceRanges.back().first+instanceRanges.back().count==i)
                ++instanceRanges.back().count;
            else instanceRanges.push_back({i,1});
            // Bound fragmented work rather than issuing hundreds of tiny copies.
            if(instanceRanges.size()>32) {full=true;break;}
        }
    }
    if(full) {
        instanceRanges.clear();instanceRanges.push_back({0,pendingInstanceCount});
    }
    for(const auto& range:instanceRanges)
        for(uint32_t i=range.first;i<range.first+range.count;++i)
            instanceScratch[i]=scene.GetTransform(static_cast<EntityID>(i)).worldMatrix;
    if(instanceRanges.empty())return true;
    if(instanceReady) {
        const RHI::ResourceBarrierDesc barrier={instanceBuffer,nullptr,RHI::ResourceState::VertexBuffer,RHI::ResourceState::CopyDestination,{}};
        commands.ResourceBarrier(&barrier,1);
    }
    for(const auto& range:instanceRanges)
        if(!device->UpdateBuffer(commands,instanceBuffer,range.first*sizeof(Math::float4x4),
            instanceScratch.data()+range.first,range.count*sizeof(Math::float4x4)))return false;
    const RHI::ResourceBarrierDesc ready={instanceBuffer,nullptr,RHI::ResourceState::CopyDestination,RHI::ResourceState::VertexBuffer,{}};
    commands.ResourceBarrier(&ready,1);
    return true;
}

void Renderer::CommitFrameData()
{
    instanceScene=pendingInstanceScene;instanceRevision=pendingInstanceRevision;instanceCount=pendingInstanceCount;
    lightingBytes.swap(pendingLightingBytes);
    if(config.lighting.enabled && config.lighting.shadows)shadowBytes.swap(pendingShadowBytes);
}

RHI::ResourceSetHandle Renderer::CachedResourceSet(RHI::PipelineHandle selectedPipeline,
    const std::vector<RHI::ResourceBinding>& bindings,std::vector<CachedSet>& cache)
{
    for(auto& entry:cache) {
        if(entry.set->GetPipeline()!=selectedPipeline || entry.set->GetBindingCount()!=bindings.size())continue;
        bool same=true;
        for(size_t i=0;i<bindings.size() && same;++i) {
            const auto& a=bindings[i];const auto& b=entry.set->GetBindings()[i];
            same=a.binding==b.binding && a.arrayElement==b.arrayElement && a.buffer==b.buffer && a.texture==b.texture &&
                a.offset==b.offset && a.size==b.size && a.subresources.firstMipLevel==b.subresources.firstMipLevel &&
                a.subresources.mipLevelCount==b.subresources.mipLevelCount &&
                a.subresources.firstArrayLayer==b.subresources.firstArrayLayer && a.subresources.arrayLayerCount==b.subresources.arrayLayerCount;
        }
        if(same) {entry.unusedFrames=0;return entry.set;}
    }
    auto* set=device->CreateResourceSet({selectedPipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(!set)return nullptr;
    try {cache.push_back({set,0});} catch(...) {device->DestroyResourceSet(set);throw;}
    return set;
}
void Renderer::PruneResourceSets(std::vector<CachedSet>& cache)
{
    // Keep the bounded live frame window so rotating Model palette/output slots
    // can reuse descriptors. RHI retains submitted references beyond eviction.
    const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
    cache.erase(std::remove_if(cache.begin(),cache.end(),[&](const CachedSet& entry){
        if(entry.unusedFrames<frameWindow)return false;device->DestroyResourceSet(entry.set);return true;
    }),cache.end());
}

namespace {
Math::Bounds3 ShadowWorldBounds(const Math::Bounds3& local,const Math::float4x4& world,std::array<double,3>& magnitude)
{
    if(!local.valid || world.m[3]!=0 || world.m[7]!=0 || world.m[11]!=0 || world.m[15]!=1)return {};
    for(float value:world.m)if(!std::isfinite(value))return {};
    const double minimum[]={local.min.x,local.min.y,local.min.z};
    const double maximum[]={local.max.x,local.max.y,local.max.z};
    double center[3],extent[3];
    for(uint32_t axis=0;axis<3;++axis) {
        if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])return {};
        center[axis]=(minimum[axis]+maximum[axis])*.5;
        extent[axis]=(maximum[axis]-minimum[axis])*.5;
    }
    float lower[3],upper[3];
    for(uint32_t row=0;row<3;++row) {
        double position=world.m[12+row],radius=0;
        magnitude[row]=std::abs(position);
        for(uint32_t column=0;column<3;++column) {
            const double coefficient=world.m[column*4+row];
            position+=coefficient*center[column];radius+=std::abs(coefficient)*extent[column];
            magnitude[row]+=std::abs(coefficient)*std::max(std::abs(minimum[column]),std::abs(maximum[column]));
        }
        // Round outwards when storing the conservative transformed box as floats.
        lower[row]=std::nextafter(static_cast<float>(position-radius),-std::numeric_limits<float>::infinity());
        upper[row]=std::nextafter(static_cast<float>(position+radius),std::numeric_limits<float>::infinity());
        if(!std::isfinite(lower[row]) || !std::isfinite(upper[row]))return {};
    }
    return {{lower[0],lower[1],lower[2]},{upper[0],upper[1],upper[2]},true};
}

struct ShadowFrustum
{
    std::array<std::array<double,4>,6> planes={},magnitudes={};
    bool valid=false;
    explicit ShadowFrustum(const Math::float4x4& matrix)
    {
        for(float value:matrix.m)if(!std::isfinite(value))return;
        for(uint32_t plane=0;plane<6;++plane) {
            const uint32_t row=plane/2;
            const double sign=plane%2?-1:1;
            for(uint32_t column=0;column<4;++column) {
                const double a=matrix.m[column*4+3],b=matrix.m[column*4+row];
                planes[plane][column]=plane==4?b:a+sign*b;
                magnitudes[plane][column]=plane==4?std::abs(b):std::abs(a)+std::abs(b);
            }
            if(planes[plane][0]==0 && planes[plane][1]==0 && planes[plane][2]==0)return;
        }
        valid=true;
    }
    bool Outside(const Math::Bounds3& bounds,const std::array<double,3>& worldMagnitude) const
    {
        if(!valid || !bounds.valid)return false;
        const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
        const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
        for(uint32_t plane=0;plane<6;++plane) {
            double distance=planes[plane][3],scale=magnitudes[plane][3];
            for(uint32_t axis=0;axis<3;++axis) {
                distance+=planes[plane][axis]*(planes[plane][axis]>=0?maximum[axis]:minimum[axis]);
                // Preserve the pre-cancellation world terms, including when a
                // shader multiplies light*world before applying the local vertex.
                scale+=magnitudes[plane][axis]*worldMagnitude[axis];
            }
            if(distance < -1e-5*std::max(1.0,scale))return true;
        }
        return false;
    }
};

bool OutsideCamera(const Math::Bounds3& bounds,const Math::float4x4& world,const Camera& camera)
{
    if(!bounds.valid)return false;
    // Unknown projective world/view contracts fail open. Standard perspective and
    // orthographic projections (including reversed axes) use homogeneous ZO planes.
    const auto affine=[](const Math::float4x4& m) {return m.m[3]==0 && m.m[7]==0 && m.m[11]==0 && m.m[15]==1;};
    const auto& p=camera.projection;
    if(!affine(world) || !affine(camera.view) ||
        !((p.m[11]==-1 && p.m[15]==0) || (p.m[11]==0 && p.m[15]==1)) ||
        p.m[1]!=0 || p.m[2]!=0 || p.m[3]!=0 || p.m[4]!=0 || p.m[6]!=0 || p.m[7]!=0 ||
        p.m[0]==0 || p.m[5]==0 || p.m[10]==0)return false;
    const auto viewProjection=p*camera.view;
    for(float v:viewProjection.m)if(!std::isfinite(v))return false;
    for(float v:world.m)if(!std::isfinite(v))return false;
    bool outside[6]={true,true,true,true,true,true};
    for(uint32_t corner=0;corner<8;++corner) {
        const double x=(corner&1)?bounds.max.x:bounds.min.x;
        const double y=(corner&2)?bounds.max.y:bounds.min.y;
        const double z=(corner&4)?bounds.max.z:bounds.min.z;
        const double point[]={x,y,z,1};
        double transformed[4]={},magnitude[4]={},clip[4]={};
        for(uint32_t row=0;row<4;++row)for(uint32_t column=0;column<4;++column) {
            const double term=world.m[column*4+row]*point[column];
            transformed[row]+=term;magnitude[row]+=std::abs(term);
        }
        double errorScale=1;
        for(uint32_t row=0;row<4;++row) {
            double sum=0;
            for(uint32_t column=0;column<4;++column) {
                const double coefficient=viewProjection.m[column*4+row];
                clip[row]+=coefficient*transformed[column];sum+=std::abs(coefficient)*magnitude[column];
            }
            errorScale=std::max(errorScale,sum);
        }
        for(double v:clip)if(!std::isfinite(v))return false;
        // Includes cancellation at large world coordinates, not just final clip size.
        const double epsilon=1e-5*errorScale;
        const double distances[]={clip[3]+clip[0],clip[3]-clip[0],clip[3]+clip[1],clip[3]-clip[1],clip[2],clip[3]-clip[2]};
        for(uint32_t plane=0;plane<6;++plane)outside[plane]=outside[plane] && distances[plane]<-epsilon;
    }
    return std::any_of(std::begin(outside),std::end(outside),[](bool value){return value;});
}
bool EqualMaterial(const MaterialDesc& a,const MaterialDesc& b)
{
    return a.baseColor.x==b.baseColor.x && a.baseColor.y==b.baseColor.y && a.baseColor.z==b.baseColor.z && a.baseColor.w==b.baseColor.w &&
        a.emissiveColor.x==b.emissiveColor.x && a.emissiveColor.y==b.emissiveColor.y && a.emissiveColor.z==b.emissiveColor.z &&
        a.metallicFactor==b.metallicFactor && a.roughnessFactor==b.roughnessFactor && a.normalScale==b.normalScale && a.occlusionStrength==b.occlusionStrength;
}
}

bool Renderer::CreateMaterialResourceSets(const Scene& scene, RHI::ICommandList& commands,
	const std::vector<RendererDrawDesc>* draws, std::vector<RHI::ResourceSetHandle>& sets,
	RHI::ResourceScope& resources)
{
	if(device == nullptr || pipeline == nullptr || lightingBuffer == nullptr) return false;
	const bool shadowsEnabled = shadowDepthTarget != nullptr;
	if(shadowsEnabled && shadowMatrixBuffer == nullptr) return false;

	if(UsesBindlessMaterials())
	{
		if(materialStates.empty()) {materialIndexBuffer=nullptr;materialIndexReady=false;return true;}
		// 기본 셰이더의 28개 텍스처 단위로 묶어 재질 수와 관계없이 바인딩한다.
        if(materialPages.empty())materialPages.emplace_back();
        for(auto& page:materialPages) {page.textures.clear();page.materials.clear();}
        uint32_t pageCount=1;
        auto& indices=materialIndexScratch;
        indices.assign(materialStates.size()*8,0);
		for(uint32_t i = 0; i < materialStates.size(); ++i)
		{
			auto& current=materialPages[pageCount-1];
            uint32_t textureCount=static_cast<uint32_t>(current.textures.size());
            // The five slots may repeat textures; count each new handle once.
            for(uint32_t slot=0;slot<kMaterialTextureCount;++slot) {
                const auto texture=materialStates[i].textures[slot];
                if(std::find(current.textures.begin(),current.textures.end(),texture)!=current.textures.end())continue;
                bool seen=false;
                for(uint32_t earlier=0;earlier<slot;++earlier)seen=seen || materialStates[i].textures[earlier]==texture;
                if(!seen)++textureCount;
            }
            if(textureCount>28) {
                ++pageCount;
                if(materialPages.size()<pageCount)materialPages.emplace_back();
            }
            auto& page=materialPages[pageCount-1];
			page.materials.push_back(i);
			for(uint32_t slot = 0; slot < kMaterialTextureCount; ++slot)
			{
				const auto texture = materialStates[i].textures[slot];
				const auto found = std::find(page.textures.begin(), page.textures.end(), texture);
				indices[i * 8 + slot] = static_cast<uint32_t>(found - page.textures.begin());
				if(found == page.textures.end()) page.textures.push_back(texture);
			}
		}
		if(indices.size() > UINT32_MAX / sizeof(uint32_t)) return false;
        const auto bytes=static_cast<uint32_t>(indices.size()*sizeof(uint32_t));
        if(!materialIndexBuffer || materialIndexBuffer->GetDesc().size!=bytes) {
            materialIndexBuffer=device->CreateBuffer({bytes,16,RHI::BufferUsage::Storage,RHI::ResourceState::CopyDestination});
            materialIndexReady=false;
        }
        if(!materialIndexBuffer)return false;
        auto* materialBuffer=materialIndexBuffer;
        if(!materialIndexReady || indices!=materialIndexBytes) {
            if(materialIndexReady) {
                const RHI::ResourceBarrierDesc barrier={materialBuffer,nullptr,RHI::ResourceState::ShaderResource,RHI::ResourceState::CopyDestination,{}};
                commands.ResourceBarrier(&barrier,1);
            }
            if(!RecordBufferUpload(device,commands,materialBuffer,indices.data(),bytes,RHI::ResourceState::ShaderResource))return false;
            materialIndexRecorded=true;
        }
		sets.assign(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
		for(uint32_t pageIndex=0;pageIndex<pageCount;++pageIndex)
        {
            const auto& page=materialPages[pageIndex];
            auto& bindings=mainBindingScratch;bindings.clear();
			for(uint32_t i = 0; i < 28; ++i)
				bindings.push_back({0, i, nullptr, page.textures[i < page.textures.size() ? i : 0], 0, 0, {}});
			bindings.push_back({1u, 0, lightingBuffer, nullptr,
				0, static_cast<uint32_t>(sizeof(RendererLightingConstants)), {}});
			bindings.push_back({29, 0, materialBuffer, nullptr, 0, materialBuffer->GetDesc().size, {}});
			if(shadowsEnabled)
			{
				bindings.push_back({28, 0, nullptr, shadowDepthTarget, 0, 0, {}});
				bindings.push_back({3u, 0, shadowMatrixBuffer, nullptr,
					0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}});
			}
			if(draws)
			{
				// 확장이 제공한 객체별 자원을 기본 재질 바인딩에 추가한다.
				for(uint32_t entity = 0; entity < scene.GetEntityCount(); ++entity)
				{
					const auto material = ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(entity)));
					if(std::find(page.materials.begin(), page.materials.end(), material) == page.materials.end()) continue;
					auto& combined=mainCombinedBindingScratch;combined.assign(bindings.begin(),bindings.end());
					const auto& extra = (*draws)[entity].vertexResources;
					combined.insert(combined.end(), extra.begin(), extra.end());
					sets[entity] = CachedResourceSet(pipeline,combined,mainSets);
					if(!sets[entity])
					{
						return false;
					}
				}
			}
			else
			{
				auto* table = CachedResourceSet(pipeline,bindings,mainSets);
				if(!table)
				{
					return false;
				}
				for(auto index : page.materials) sets[index] = table;
			}
		}
		return true;
	}

	sets.resize(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
	for(uint32_t setIndex = 0; setIndex < sets.size(); ++setIndex)
	{
		const auto materialIndex = draws
			? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(setIndex))) : setIndex;
		if(materialIndex >= materialStates.size()) continue;
		const auto& material = materialStates[materialIndex];
		auto& bindings=mainBindingScratch;bindings = {
			{0u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::BaseColor)], 0, 0, {}},
			{1u, 0, lightingBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererLightingConstants)), {}},
			{4u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::MetallicRoughness)], 0, 0, {}},
			{5u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Normal)], 0, 0, {}},
			{6u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Occlusion)], 0, 0, {}},
			{7u, 0, nullptr, material.textures[ToIndex(MaterialTextureKind::Emissive)], 0, 0, {}},
			{3u, 0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}},
			{2u, 0, nullptr, shadowDepthTarget, 0, 0, {}}
		};
		if(!shadowsEnabled) bindings.resize(bindings.size() - 2);
		if(draws)
		{
			const auto& extra = (*draws)[setIndex].vertexResources;
			bindings.insert(bindings.end(), extra.begin(), extra.end());
		}
		sets[setIndex] = CachedResourceSet(pipeline,bindings,mainSets);
		if(!sets[setIndex])
		{
			return false;
		}
	}
	return true;
}

void Renderer::DestroyMeshState(RHI::IDevice* device, SceneMeshState& mesh)
{
	DestroyBuffer(device, mesh.vertexBuffer, mesh.vertexReady);
	DestroyBuffer(device, mesh.indexBuffer, mesh.indexReady);
	mesh.indexCount = 0;
    mesh.bounds={};
	mesh.prepared = false;
}

// 공개 RHI가 업로드 바이트를 복사하므로 메시별 변환 배열은 기록 직후 해제할 수 있다.
bool Renderer::PrepareGeometry(const Scene& scene, RHI::IDevice* device)
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
		if(mesh.prepared) continue;
        const MeshData& source = *sourceMesh;
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
	}

	if(!commands) return true;
	const bool closed = commands->Close();
	const bool submitted = closed && device->Submit(&commands, 1);
	if(submitted)
	{
		// 실패한 제출은 준비 완료로 표시하지 않아 다음 프레임에서 다시 업로드한다.
		for(bool* ready : uploaded) *ready = true;
		for(auto& mesh : m_meshes)
			if(mesh.vertexReady && mesh.indexReady) mesh.prepared = true;
	}
	return submitted && !uploadFailed;
}

bool Renderer::RecordShadowPass(const Scene& scene, const Camera& camera, const ShadowData& shadows,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TimestampQueryHandle shadowQuery)
{
	RHI::ResourceScope resources(*device);
	const bool stock=UsesStockGeometry() && !draws;
	const bool instanced=UsesInstanceStream();
	const bool cull=stock && config.enableFrustumCulling;
	if(cull) {
        shadowBoundsScratch.resize(scene.GetEntityCount());
        for(uint32_t i=0;i<scene.GetEntityCount();++i) {
            auto& bounds=shadowBoundsScratch[i];bounds={};
            const auto entity=static_cast<EntityID>(i);
            const auto meshId=scene.GetEntityMesh(entity);
            if(scene.GetEntityLighting(entity).castShadow && IsValid(meshId) && ToIndex(meshId)<m_meshes.size())
                bounds.bounds=ShadowWorldBounds(m_meshes[ToIndex(meshId)].bounds,scene.GetTransform(entity).worldMatrix,bounds.magnitude);
        }
    }
	auto& shadowSets=shadowSetScratch;
	{
		commands.BeginDebugEvent("Shadow");
		shadowSets.resize(draws ? scene.GetEntityCount() : 1, nullptr);
		for(uint32_t i = 0; i < shadowSets.size(); ++i)
		{
			auto& bindings=shadowBindingScratch;bindings = {{3u,
				0, shadowMatrixBuffer, nullptr, 0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}}};
			if(draws)
			{
				const auto& extra = (*draws)[i].vertexResources;
				bindings.insert(bindings.end(), extra.begin(), extra.end());
			}
			shadowSets[i] = CachedResourceSet(shadowPipeline,bindings,this->shadowSets);
			if(!shadowSets[i])
			{
				return false;
			}
		}
	}
	const auto viewProjection = camera.projection * camera.view;
	if(shadowQuery) { commands.ResetTimestamps(shadowQuery, 0, 2); commands.WriteTimestamp(shadowQuery, 0); }
	{
		RHI::DepthStencilAttachment depth;
		depth.texture = shadowDepthTarget;
		depth.state = RHI::ResourceState::DepthWrite;
		depth.depthLoadOp = RHI::LoadOp::Clear;
		depth.depthStoreOp = RHI::StoreOp::Store;
		commands.BeginRendering({nullptr, 0, &depth});
		commands.BindGraphicsPipeline(shadowPipeline);
		for(uint32_t view = 0; view < shadows.viewCount; ++view)
		{
			const ShadowFrustum frustum(shadows.constants.lightViewProjectionMatrix[view]);
			const auto outside=[&](uint32_t index) {
                const auto& bounds=shadowBoundsScratch[index];
                return frustum.Outside(bounds.bounds,bounds.magnitude);
            };
			const uint32_t x = (view % shadows.columns) * shadows.resolution;
			const uint32_t y = (view / shadows.columns) * shadows.resolution;
			commands.SetViewport({static_cast<float>(x), static_cast<float>(y),
				static_cast<float>(shadows.resolution), static_cast<float>(shadows.resolution), 0, 1});
			commands.SetScissor({static_cast<int32_t>(x), static_cast<int32_t>(y), shadows.resolution, shadows.resolution});
			for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
			{
				const auto entity = static_cast<EntityID>(entityIndex);
				const auto& lighting = scene.GetEntityLighting(entity);
				if(!lighting.castShadow) continue;
				const auto meshId = scene.GetEntityMesh(entity);
				const auto materialId = scene.GetEntityMaterial(entity);
				if(!IsValid(meshId) || !IsValid(materialId)) continue;
				const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
				if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size()) continue;
				const auto& mesh = m_meshes[meshIndex];
				if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
				if(cull && outside(entityIndex))continue;
				uint32_t count=1;
				if(stock && instanced) {
                    // The stock depth shader only consumes the mesh, instance matrix
                    // and light view. Materials cannot change its depth output.
                    while(entityIndex+count<scene.GetEntityCount()) {
                        const auto next=static_cast<EntityID>(entityIndex+count);
                        const auto material=scene.GetEntityMaterial(next);
                        if(scene.GetEntityMesh(next)!=meshId || !scene.GetEntityLighting(next).castShadow ||
                            !IsValid(material) || ToIndex(material)>=materialStates.size() ||
                            (cull && outside(entityIndex+count)))break;
                        ++count;
                    }
                }
				const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
				commands.BindResourceSet(shadowSets[input ? entityIndex : 0]);
				commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
				if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*sizeof(Math::float4x4));
				commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
				const uint32_t flags = materialStates[materialIndex].textureFlags |
					(lighting.receiveShadow ? 32u : 0u);
				auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
				constants.padding2 = view;
				commands.SetInlineConstants(0, sizeof(constants), &constants);
				if(input && !input->inlineConstants.empty())
					commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
				commands.DrawIndexedInstanced(mesh.indexCount,count,0,0,0);
				entityIndex+=count-1;
			}
		}
		commands.EndRendering();

	}
	if(shadowQuery) commands.WriteTimestamp(shadowQuery, 1);
	commands.EndDebugEvent();
	return true;
}

bool Renderer::RecordMainPass(const Scene& scene, const Camera& camera,
	RHI::ICommandList& commands, const std::vector<RendererDrawDesc>* draws,
	RHI::TimestampQueryHandle mainQuery, RHI::TextureHandle output)
{
	RHI::ResourceScope resources(*device);
	auto& materialSets=mainSetScratch;
    struct MaterialBufferTransaction {
        Renderer& r;
        RHI::BufferHandle previous;
        bool ready,committed=false;
        explicit MaterialBufferTransaction(Renderer& owner):r(owner),previous(r.materialIndexBuffer),ready(r.materialIndexReady) {}
        ~MaterialBufferTransaction() {
            if(committed) {if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(previous);}
            else {
                if(previous!=r.materialIndexBuffer)r.device->DestroyBuffer(r.materialIndexBuffer);
                r.materialIndexBuffer=previous;r.materialIndexReady=ready;
            }
        }
    } materialTransaction(*this);
	if(!CreateMaterialResourceSets(scene, commands, draws, materialSets, resources)) return false;
    materialTransaction.committed=true;
	auto* target = config.enableHdrRendering ? hdrTarget : output;
	const auto viewProjection = camera.projection * camera.view;
	if(mainQuery) { commands.ResetTimestamps(mainQuery, 0, 2); commands.WriteTimestamp(mainQuery, 0); }
	commands.BeginDebugEvent("MainForward");

	RHI::ColorAttachment color;
	color.texture = target;
	color.loadOp = RHI::LoadOp::Clear;
	color.storeOp = RHI::StoreOp::Store;
	color.clearColor[0] = config.clearColor.x;
	color.clearColor[1] = config.clearColor.y;
	color.clearColor[2] = config.clearColor.z;
	color.clearColor[3] = config.clearColor.w;
    // clearColor is a display color, matching the existing window/Canvas path.
    if(RHI::IsSrgbFormat(target->GetDesc().format))
        for(uint32_t channel=0;channel<3;++channel)
        {
            const float value=color.clearColor[channel];
            color.clearColor[channel]=value<=0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
        }
	RHI::DepthStencilAttachment depth;
	depth.texture = depthStencilTarget;
	depth.state = RHI::ResourceState::DepthWrite;
	depth.depthLoadOp = RHI::LoadOp::Clear;
	depth.depthStoreOp = RHI::StoreOp::Discard;
	if(depthStencilTarget->GetDesc().format == RHI::Format::D24_UNORM_S8_UINT)
	{
		depth.stencilLoadOp = RHI::LoadOp::Discard;
		depth.stencilStoreOp = RHI::StoreOp::Discard;
	}
	commands.BeginRendering({&color, 1, &depth});
	commands.BindGraphicsPipeline(pipeline);
	commands.SetViewport({0, 0, static_cast<float>(target->GetDesc().width), static_cast<float>(target->GetDesc().height), 0, 1});
	commands.SetScissor({0, 0, target->GetDesc().width, target->GetDesc().height});
    const bool stock=UsesStockGeometry() && !draws;
    const bool instanced=UsesInstanceStream();
	const bool batchBindings = shaderSources.bytes[MeshFragment].empty();
	RHI::ResourceSetHandle boundMaterial = nullptr;
	for(uint32_t entityIndex = 0; entityIndex < scene.GetEntityCount(); ++entityIndex)
	{
		const auto entity = static_cast<EntityID>(entityIndex);
		const auto meshId = scene.GetEntityMesh(entity);
		const auto materialId = scene.GetEntityMaterial(entity);
		if(!IsValid(meshId) || !IsValid(materialId)) continue;
		const uint32_t meshIndex = ToIndex(meshId), materialIndex = ToIndex(materialId);
		const uint32_t setIndex = draws ? entityIndex : materialIndex;
		if(meshIndex >= m_meshes.size() || materialIndex >= materialStates.size() || setIndex >= materialSets.size()) continue;
		const auto& mesh = m_meshes[meshIndex];
		if(!mesh.prepared || !mesh.vertexBuffer || !mesh.indexBuffer || !mesh.indexCount) continue;
        if(stock && config.enableFrustumCulling && OutsideCamera(mesh.bounds,scene.GetTransform(entity).worldMatrix,camera)) {
            ++recordingStatistics.culledEntityCount;continue;
        }
        uint32_t count=1;
        if(stock && instanced) {
            while(entityIndex+count<scene.GetEntityCount()) {
                const auto next=static_cast<EntityID>(entityIndex+count);
                const auto nextMaterial=scene.GetEntityMaterial(next);
                if(scene.GetEntityMesh(next)!=meshId || !IsValid(nextMaterial) || ToIndex(nextMaterial)>=materialStates.size())break;
                const auto index=ToIndex(nextMaterial);
                if(scene.GetEntityLighting(next).receiveShadow!=scene.GetEntityLighting(entity).receiveShadow ||
                    materialStates[index].textures!=materialStates[materialIndex].textures ||
                    materialStates[index].textureFlags!=materialStates[materialIndex].textureFlags ||
                    !EqualMaterial(scene.Materials()[index],scene.Materials()[materialIndex]) ||
                    (config.enableFrustumCulling && OutsideCamera(mesh.bounds,scene.GetTransform(next).worldMatrix,camera)))break;
                ++count;
            }
        }
        if(instanced)commands.BindVertexBuffer(1,instanceBuffer,entityIndex*sizeof(Math::float4x4));
		if(!batchBindings || boundMaterial != materialSets[setIndex])
		{
			boundMaterial = materialSets[setIndex];
			commands.BindResourceSet(boundMaterial);
		}
		const auto* input = draws ? &(*draws)[entityIndex] : nullptr;
		commands.BindVertexBuffer(0, input && input->vertexBuffer ? input->vertexBuffer : mesh.vertexBuffer, 0);
		commands.BindIndexBuffer(mesh.indexBuffer, RHI::Format::R32_UINT, 0);
		const uint32_t flags = materialStates[materialIndex].textureFlags |
			(scene.GetEntityLighting(entity).receiveShadow ? 32u : 0u);
		auto constants = MakeDrawConstants(viewProjection, scene.Materials()[materialIndex], scene.GetTransform(entity), flags);
		constants.padding2 = materialIndex;
		commands.SetInlineConstants(0, sizeof(constants), &constants);
		if(input && !input->inlineConstants.empty())
			commands.SetInlineConstants(sizeof(constants), static_cast<uint32_t>(input->inlineConstants.size()), input->inlineConstants.data());
        commands.DrawIndexedInstanced(mesh.indexCount,count,0,0,0);
        ++recordingStatistics.mainDrawCount;recordingStatistics.mainInstanceCount+=count;
        entityIndex+=count-1;
	}
	commands.EndRendering();
	commands.EndDebugEvent();
	return true;
}

void Renderer::ReleaseGeometry(RHI::IDevice* device)
{
	if(device == nullptr) return;
	for(auto& mesh : m_meshes) DestroyMeshState(device, mesh);
	m_meshes.clear();
}
}
