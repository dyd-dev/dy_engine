#include "RendererInternal.h"
#include "DrawCommands.h"
#include "BasicEnvironmentResources.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "../RHI/ResourceSet.h"
#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace dyf
{
RHI::ResourceSetHandle Renderer::Impl::CachedResourceSet(RHI::PipelineHandle selectedPipeline,
    const std::vector<RHI::ResourceBinding>& bindings,std::vector<CachedSet>& cache)
{
    size_t keyHash=std::hash<RHI::PipelineHandle>{}(selectedPipeline);
    const auto combine=[&](size_t value) {keyHash^=value+size_t(0x9e3779b9u)+(keyHash<<6)+(keyHash>>2);};
    combine(bindings.size());
    for(const auto& binding:bindings) {
        combine(binding.binding);combine(binding.arrayElement);
        combine(std::hash<RHI::BufferHandle>{}(binding.buffer));
        combine(std::hash<RHI::TextureHandle>{}(binding.texture));
        combine(binding.offset);combine(binding.size);
        combine(binding.subresources.firstMipLevel);combine(binding.subresources.mipLevelCount);
        combine(binding.subresources.firstArrayLayer);combine(binding.subresources.arrayLayerCount);
    }
    const auto first=std::lower_bound(cache.begin(),cache.end(),keyHash,
        [](const CachedSet& entry,size_t hash) {return entry.keyHash<hash;});
    // 해시는 후보만 좁힌다. 충돌은 전체 바인딩을 비교해 구분한다.
    for(auto candidate=first;config.enableResourceSetCache && candidate!=cache.end() && candidate->keyHash==keyHash;++candidate) {
        auto& entry=*candidate;
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
    try {cache.insert(first,{set,0,keyHash});} catch(...) {device->DestroyResourceSet(set);throw;}
    return set;
}
void Renderer::Impl::PruneResourceSets(std::vector<CachedSet>& cache)
{
    // 진행 중인 프레임 구간에서는 팔레트·출력 슬롯의 디스크립터를 재사용한다.
    // 캐시에서 제거해도 제출된 참조의 수명은 RHI가 유지한다.
    const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
    cache.erase(std::remove_if(cache.begin(),cache.end(),[&](const CachedSet& entry){
        if(config.enableResourceSetCache && entry.unusedFrames<frameWindow)return false;device->DestroyResourceSet(entry.set);return true;
    }),cache.end());
}

bool Renderer::Impl::CreateMaterialResourceSets(const Scene& scene, RHI::ICommandList& commands,
	const std::vector<RendererDrawDesc>* draws, std::vector<RHI::ResourceSetHandle>& sets)
{
	if(device == nullptr || pipeline == nullptr || lightingBuffer == nullptr) return false;
	if(stockDrawInputs) draws=nullptr;
	const bool shadowsEnabled = shadowDepthTarget != nullptr;
	if(shadowsEnabled && shadowMatrixBuffer == nullptr) return false;

	if(UsesBindlessMaterials())
	{
		if(materialStates.empty()) {materialIndexBuffer=nullptr;materialIndexReady=false;return true;}
        const RenderEnvironmentResources* environment=&frameLighting.environment;
        RenderEnvironmentResources neutralEnvironment;
        const bool advanced=UsesAdvancedRendering();
        if(!advanced)
        {
            // Legacy basic paging did not consume environment/AO resources.
            // Neutral inputs preserve that result, including custom providers.
            if(!basicEnvironment)return false;
            neutralEnvironment=basicEnvironment->GetResources(defaultMaterialTextures[2]);
            environment=&neutralEnvironment;
        }
        if(!environment->constantBuffer)return false;
		// 클러스터 조명 슬롯 앞의 19개 텍스처 단위로 재질을 묶는다.
        if(materialPages.empty())materialPages.emplace_back();
        for(auto& page:materialPages) {page.textures.clear();page.materials.clear();}
        uint32_t pageCount=1;
        auto& indices=materialIndexScratch;
        indices.assign(materialStates.size()*8,0);
		for(uint32_t i = 0; i < materialStates.size(); ++i)
		{
			auto& current=materialPages[pageCount-1];
            uint32_t textureCount=static_cast<uint32_t>(current.textures.size());
            // 다섯 슬롯에 같은 텍스처가 반복되어도 고유 핸들은 한 번만 센다.
            for(uint32_t slot=0;slot<materialStates[i].textures.size();++slot) {
                const auto texture=materialStates[i].textures[slot];
                if(std::find(current.textures.begin(),current.textures.end(),texture)!=current.textures.end())continue;
                bool seen=false;
                for(uint32_t earlier=0;earlier<slot;++earlier)seen=seen || materialStates[i].textures[earlier]==texture;
                if(!seen)++textureCount;
            }
            if(textureCount>19) {
                ++pageCount;
                if(materialPages.size()<pageCount)materialPages.emplace_back();
            }
            auto& page=materialPages[pageCount-1];
			page.materials.push_back(i);
			for(uint32_t slot = 0; slot < materialStates[i].textures.size(); ++slot)
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
			for(uint32_t i = 0; i < 19; ++i)
				bindings.push_back({0, i, nullptr, page.textures[i < page.textures.size() ? i : 0], 0, 0, {}});
			bindings.push_back({1u, 0, lightingBuffer, nullptr,
				0, static_cast<uint32_t>(sizeof(RendererLightingConstants)), {}});
			bindings.push_back({29, 0, materialBuffer, nullptr, 0, materialBuffer->GetDesc().size, {}});
            bindings.push_back({19,0,frameLighting.clusterLights.buffer,nullptr,0,frameLighting.clusterLights.buffer->GetDesc().size,{}});
            bindings.push_back({20,0,frameLighting.clusterList.buffer,nullptr,0,frameLighting.clusterList.buffer->GetDesc().size,{}});
            bindings.push_back({21,0,frameLighting.clusterConstants.buffer,nullptr,0,sizeof(RendererClusterConstants),{}});
            bindings.push_back({26,0,nullptr,frameLighting.areaMatrix.texture ? frameLighting.areaMatrix.texture : defaultMaterialTextures[2],0,0,{}});
            bindings.push_back({27,0,nullptr,frameLighting.areaAmplitude.texture ? frameLighting.areaAmplitude.texture : defaultMaterialTextures[2],0,0,{}});
            bindings.push_back({22,0,nullptr,advanced && frameAmbientOcclusion ? frameAmbientOcclusion : defaultMaterialTextures[0],0,0,{}});
            bindings.push_back({23,0,nullptr,environment->specular.texture,0,0,{}});
            bindings.push_back({24,0,nullptr,environment->diffuse.texture,0,0,{}});
            bindings.push_back({14,0,environment->constantBuffer,nullptr,0,sizeof(RenderEnvironmentConstants),{}});
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
					sets[entity] = CachedResourceSet(MaterialPipeline(scene.Materials()[material]),combined,mainSets);
					if(!sets[entity])
					{
						return false;
					}
				}
			}
			else
			{
				std::array<RHI::PipelineHandle,4> pagePipelines={};
				std::array<RHI::ResourceSetHandle,4> pageSets={};
				uint32_t pageSetCount=0;
				for(auto index : page.materials)
				{
					const auto selectedPipeline=MaterialPipeline(scene.Materials()[index]);
					RHI::ResourceSetHandle selectedSet=nullptr;
					if(config.enableResourceSetCache)
						for(uint32_t slot=0;slot<pageSetCount;++slot)
							if(pagePipelines[slot]==selectedPipeline) {selectedSet=pageSets[slot];break;}
					if(!selectedSet)
					{
						selectedSet=CachedResourceSet(selectedPipeline,bindings,mainSets);
						if(!selectedSet) return false;
						if(config.enableResourceSetCache && pageSetCount<pagePipelines.size())
						{
							pagePipelines[pageSetCount]=selectedPipeline;
							pageSets[pageSetCount++]=selectedSet;
						}
					}
					sets[index]=selectedSet;
				}
			}
		}
		return true;
	}

	sets.resize(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
	const bool separateTransparency=m_environmentCapture || wireframeActive || UsesSeparateTransparency();
	for(uint32_t setIndex = 0; setIndex < sets.size(); ++setIndex)
	{
		const auto materialIndex = draws
			? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(setIndex))) : setIndex;
		if(materialIndex >= materialStates.size()) continue;
		if(separateTransparency && UsesBlendedRendering(scene.Materials()[materialIndex]))
		{
			sets[setIndex]=nullptr;
			continue;
		}
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
		if(shaderSources.bytes[MeshFragment].empty())
		{
            if(!frameLighting.environment.constantBuffer) return false;
            bindings.push_back({16,0,nullptr,frameAmbientOcclusion ? frameAmbientOcclusion : defaultMaterialTextures[0],0,0,{}});
            bindings.push_back({13,0,nullptr,frameLighting.environment.specular.texture,0,0,{}});
            bindings.push_back({17,0,nullptr,frameLighting.environment.diffuse.texture,0,0,{}});
            bindings.push_back({14,0,frameLighting.environment.constantBuffer,nullptr,0,sizeof(RenderEnvironmentConstants),{}});
            bindings.push_back({19,0,frameLighting.clusterLights.buffer,nullptr,0,frameLighting.clusterLights.buffer->GetDesc().size,{}});
            bindings.push_back({20,0,frameLighting.clusterList.buffer,nullptr,0,frameLighting.clusterList.buffer->GetDesc().size,{}});
            bindings.push_back({21,0,frameLighting.clusterConstants.buffer,nullptr,0,sizeof(RendererClusterConstants),{}});
            bindings.push_back({26,0,nullptr,frameLighting.areaMatrix.texture ? frameLighting.areaMatrix.texture : defaultMaterialTextures[2],0,0,{}});
            bindings.push_back({27,0,nullptr,frameLighting.areaAmplitude.texture ? frameLighting.areaAmplitude.texture : defaultMaterialTextures[2],0,0,{}});
		}
		if(draws)
		{
			const auto& extra = (*draws)[setIndex].vertexResources;
			bindings.insert(bindings.end(), extra.begin(), extra.end());
		}
		sets[setIndex] = CachedResourceSet(MaterialPipeline(scene.Materials()[materialIndex]),bindings,mainSets);
		if(!sets[setIndex])
		{
			return false;
		}
	}
	return true;
}
}
