#include "RendererInternal.h"
#include "SceneAlgorithms.h"
#include "DrawCommands.h"
#include "BasicEnvironmentResources.h"
#include "dyf/Scene.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/ResourceSet.h"
#include <algorithm>
#include <array>
#include <functional>
#include <vector>

namespace dyf
{
// 파이프라인과 바인딩의 위치·자원·범위·서브리소스를 해시로 좁혀 찾는다. 같은 해시 후보도 전체 내용을 비교해야 재사용할 수 있다.
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
    // cache를 keyHash 정렬 순서로 유지하므로 같은 해시 구간만 검사한다. 캐시 OFF일 때도 새 세트를 추적 배열에 넣어 수명 정리를 유지한다.
    const auto first=std::lower_bound(cache.begin(),cache.end(),keyHash,
        [](const CachedSet& entry,size_t hash) {return entry.keyHash<hash;});
    // 해시는 후보만 좁힌다. 충돌은 전체 바인딩을 비교해 구분한다.
    for(auto candidate=first;config.enableResourceSetCache && candidate!=cache.end() && candidate->keyHash==keyHash;++candidate) {
        auto& entry=*candidate;
        if(entry.set->GetPipeline()!=selectedPipeline || entry.set->GetBindingCount()!=bindings.size())continue; // 파이프라인 또는 바인딩 개수가 다르면 해시가 같아도 다른 세트다.
        bool same=true;
        for(size_t i=0;i<bindings.size() && same;++i) {
            const auto& a=bindings[i];const auto& b=entry.set->GetBindings()[i];
            same=a.binding==b.binding && a.arrayElement==b.arrayElement && a.buffer==b.buffer && a.texture==b.texture &&
                a.offset==b.offset && a.size==b.size && a.subresources.firstMipLevel==b.subresources.firstMipLevel &&
                a.subresources.mipLevelCount==b.subresources.mipLevelCount &&
                a.subresources.firstArrayLayer==b.subresources.firstArrayLayer && a.subresources.arrayLayerCount==b.subresources.arrayLayerCount;
        }
        if(same) {entry.unusedFrames=0;return entry.set;} // 모든 바인딩 필드가 같으면 사용 이력을 0으로 갱신하고 기존 세트를 반환한다.
    }
    auto* set=device->CreateResourceSet({selectedPipeline,bindings.data(),static_cast<uint32_t>(bindings.size())});
    if(!set)return nullptr; // RHI 세트 생성 실패는 nullptr로 전달한다.
    // 생성한 세트를 캐시에 넣다가 메모리 할당 예외가 나면 새 세트를 먼저 해제하고 예외를 다시 전파한다.
    try {cache.insert(first,{set,0,keyHash});} catch(...) {device->DestroyResourceSet(set);throw;}
    return set;
}
// 최대 동시 진행 프레임 구간 동안 사용 이력을 남긴다. 캐시 OFF 또는 오래 사용하지 않은 세트는 캐시에서 빼고 제출 참조의 최종 수명은 RHI에 맡긴다.
void Renderer::Impl::PruneResourceSets(std::vector<CachedSet>& cache)
{
    // 진행 중인 프레임 구간에서는 팔레트·출력 슬롯의 디스크립터를 재사용한다.
    const auto frameWindow=std::max(1u,device->GetDesc().maxFramesInFlight);
    cache.erase(std::remove_if(cache.begin(),cache.end(),[&](const CachedSet& entry){
        if(config.enableResourceSetCache && entry.unusedFrames<frameWindow)return false;device->DestroyResourceSet(entry.set);return true; // 캐시 ON이고 보존 구간 안이면 유지하고, 그 밖은 해제 후 배열에서 제거한다.
    }),cache.end());
}

// 표준 입력은 재질별 세트를 공유하고 사용자 정점 자원이 있으면 객체별 세트를 만든다. 실패 시 호출부가 재질 인덱스 버퍼 교체를 복구한다.
bool Renderer::Impl::CreateMaterialResourceSets(const Scene& scene, RHI::ICommandList& commands,
	const std::vector<RendererDrawDesc>* draws, std::vector<RHI::ResourceSetHandle>& sets)
{
	if(device == nullptr || pipeline == nullptr || lightingBuffer == nullptr) return false; // 필수 장치·파이프라인·조명 상수가 없으면 세트를 구성할 수 없다.
	if(stockDrawInputs) draws=nullptr; // 표준 호환 입력은 객체별 추가 자원을 쓰지 않으므로 재질별 세트를 공유한다.
	const bool shadowsEnabled = shadowDepthTarget != nullptr;
	if(shadowsEnabled && shadowMatrixBuffer == nullptr) return false; // 그림자 타깃이 있는데 상수가 없으면 불완전한 세트를 만들지 않는다.

	if(UsesBindlessMaterials()) // 설정과 장치 지원을 통과한 경우만 텍스처 페이지 경로를 사용한다.
	{
		if(materialStates.empty()) {materialIndexBuffer=nullptr;materialIndexReady=false;return true;} // 재질이 없으면 재질 인덱스 버퍼 연결을 비우고 준비를 생략한다.
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
        if(materialPages.empty())materialPages.emplace_back(); // 첫 페이지가 없으면 임시 저장소를 하나 마련한다.
        for(auto& page:materialPages) {page.textures.clear();page.materials.clear();}
        uint32_t pageCount=1;
        auto& indices=materialIndexScratch;
        // 재질마다 8개 uint 슬롯을 확보하고 텍스처 다섯 종류의 페이지 내 위치를 채운다. 남는 슬롯은 정렬/레이아웃 계약에 맞춰 0으로 둔다.
        indices.assign(materialStates.size()*8,0);
		// 현재 페이지에 새로 필요한 고유 텍스처 수를 먼저 센다. 같은 재질의 여러 슬롯이 같은 핸들이어도 한 번만 센다.
		for(uint32_t i = 0; i < materialStates.size(); ++i)
		{
			auto& current=materialPages[pageCount-1];
            uint32_t textureCount=static_cast<uint32_t>(current.textures.size());
            for(uint32_t slot=0;slot<materialStates[i].textures.size();++slot) {
                const auto texture=materialStates[i].textures[slot];
                if(std::find(current.textures.begin(),current.textures.end(),texture)!=current.textures.end())continue; // 이미 현재 페이지에 있는 텍스처는 신규 개수에 포함하지 않는다.
                bool seen=false;
                for(uint32_t earlier=0;earlier<slot;++earlier)seen=seen || materialStates[i].textures[earlier]==texture;
                if(!seen)++textureCount; // 같은 재질의 앞 슬롯에도 없었던 핸들만 추가로 센다.
            }
            if(textureCount>19) { // 현재 재질을 합치면 19개 슬롯을 넘으므로 새 페이지를 사용한다.
                ++pageCount;
                if(materialPages.size()<pageCount)materialPages.emplace_back(); // 재사용 가능한 페이지 저장소가 모자랄 때만 늘린다.
            }
            auto& page=materialPages[pageCount-1];
			page.materials.push_back(i);
			for(uint32_t slot = 0; slot < materialStates[i].textures.size(); ++slot)
			{
				const auto texture = materialStates[i].textures[slot];
				const auto found = std::find(page.textures.begin(), page.textures.end(), texture);
				indices[i * 8 + slot] = static_cast<uint32_t>(found - page.textures.begin());
				if(found == page.textures.end()) page.textures.push_back(texture); // 페이지에 처음 등장한 텍스처만 목록에 넣고 해당 위치를 인덱스로 사용한다.
			}
		}
		if(indices.size() > UINT32_MAX / sizeof(uint32_t)) return false; // 재질 인덱스 버퍼 바이트 수가 32비트를 넘으면 실패한다.
        const auto bytes=static_cast<uint32_t>(indices.size()*sizeof(uint32_t));
        if(!materialIndexBuffer || materialIndexBuffer->GetDesc().size!=bytes) { // 버퍼가 없거나 크기가 바뀌면 새 버퍼를 만들고 업로드 필요 상태로 둔다.
            materialIndexBuffer=device->CreateBuffer({bytes,16,RHI::BufferUsage::Storage,RHI::ResourceState::CopyDestination});
            materialIndexReady=false;
        }
        if(!materialIndexBuffer)return false; // 재질 인덱스 버퍼 생성 실패를 전파한다.
        // 이번 기록에서 사용할 버퍼를 고정한다. materialIndexRecorded는 기록 여부이며 ready/기준 바이트 확정은 SceneRendering의 draw 제출 성공 뒤에 한다.
        auto* materialBuffer=materialIndexBuffer;
        if(!materialIndexReady || indices!=materialIndexBytes) { // 새 버퍼이거나 마지막 제출 성공 바이트와 다를 때만 업로드를 기록한다.
            if(materialIndexReady) { // 기존 셰이더 읽기 버퍼는 복사 목적 상태로 전환한 뒤 갱신한다.
                const RHI::ResourceBarrierDesc barrier={materialBuffer,nullptr,RHI::ResourceState::ShaderResource,RHI::ResourceState::CopyDestination,{}};
                commands.ResourceBarrier(&barrier,1);
            }
            if(!RecordBufferUpload(device,commands,materialBuffer,indices.data(),bytes,RHI::ResourceState::ShaderResource))return false; // 재질 인덱스 업로드 기록 실패면 세트 준비도 실패한다.
            materialIndexRecorded=true;
        }
		sets.assign(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
        // Build stable per-material entity chains once. A page may contain many
        // materials sharing the same textures, so scanning every entity and then
        // searching page.materials makes custom geometry preparation quadratic.
        std::vector<uint32_t> firstEntity,nextEntity;
        if(draws)
        {
            firstEntity.assign(materialStates.size(),UINT32_MAX);
            nextEntity.resize(scene.GetEntityCount());
            for(uint32_t remaining=scene.GetEntityCount();remaining;--remaining)
            {
                const uint32_t entity=remaining-1;
                const auto material=ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(entity)));
                if(material>=firstEntity.size())continue;
                nextEntity[entity]=firstEntity[material];
                firstEntity[material]=entity;
            }
        }
		// 페이지마다 텍스처 배열·조명·재질 인덱스·지역광·면적광 입력을 묶는다. 비어 있는 배열 슬롯은 페이지 첫 텍스처로 채워 유효 핸들을 유지한다.
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
			if(shadowsEnabled) // 활성 그림자 타깃과 상수를 bindless 전용 슬롯에 추가한다.
			{
				bindings.push_back({28, 0, nullptr, shadowDepthTarget, 0, 0, {}});
				bindings.push_back({3u, 0, shadowMatrixBuffer, nullptr,
					0, static_cast<uint32_t>(sizeof(RendererShadowConstants)), {}});
			}
			if(draws) // 객체별 추가 정점 자원이 있으면 페이지 바인딩에 합쳐 객체별 세트를 만든다.
			{
				for(const auto material:page.materials)
				for(uint32_t entity=firstEntity[material];entity!=UINT32_MAX;entity=nextEntity[entity])
				{
					auto& combined=mainCombinedBindingScratch;combined.assign(bindings.begin(),bindings.end());
					const auto& extra = (*draws)[entity].vertexResources;
					combined.insert(combined.end(), extra.begin(), extra.end());
					sets[entity] = CachedResourceSet(MaterialPipeline(scene.Materials()[material]),combined,mainSets);
					if(!sets[entity]) // 객체별 세트 생성 실패면 전체 재질 준비를 실패 처리한다.
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

	// 여기부터 일반 바인딩 경로다. 사용자 객체별 입력이면 세트 인덱스는 객체 ID, 아니면 재질 ID다.
	sets.resize(draws ? scene.GetEntityCount() : materialStates.size(), nullptr);
	const bool separateTransparency=m_environmentCapture || wireframeActive || UsesSeparateTransparency();
	for(uint32_t setIndex = 0; setIndex < sets.size(); ++setIndex)
	{
		const auto materialIndex = draws
			? ToIndex(scene.GetEntityMaterial(static_cast<EntityID>(setIndex))) : setIndex;
		if(materialIndex >= materialStates.size()) continue; // 유효 범위 밖 재질이면 세트를 연결하지 않는다.
		if(separateTransparency && UsesBlendedRendering(scene.Materials()[materialIndex])) // 별도 투명 처리 또는 캡처/선 진단 경로에서는 블렌드·투과 재질의 메인 세트를 만들지 않는다.
		{
			sets[setIndex]=nullptr;
			continue;
		}
		const auto& material = materialStates[materialIndex];
		// 표면에 수직인 방향(법선)을 담은 텍스처도 재질 입력으로 전달한다.
		// 기본색·금속/거칠기·법선·차폐·발광 텍스처와 조명 상수를 묶고, 그림자를 사용할 때만 그림자 상수·깊이 입력을 남긴다.
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
		if(!shadowsEnabled) bindings.resize(bindings.size() - 2); // 그림자 미사용이면 기본 배열 끝의 그림자 상수/깊이 바인딩 두 개를 제거한다.
		if(shaderSources.bytes[MeshFragment].empty()) // 기본 fragment에만 주변 형상에 가려 주변광이 덜 닿는 정도를 나타내는 값(AO)의 텍스처와 환경·지역광·면적광 입력을 보충한다.
		{
            if(!frameLighting.environment.constantBuffer) return false; // 기본 셰이더에 필수인 환경 상수가 없으면 불완전한 바인딩을 만들지 않는다.
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
		if(draws) // 일반 경로의 객체별 추가 정점 자원을 기본 바인딩 뒤에 붙인다.
		{
			const auto& extra = (*draws)[setIndex].vertexResources;
			bindings.insert(bindings.end(), extra.begin(), extra.end());
		}
		sets[setIndex] = CachedResourceSet(MaterialPipeline(scene.Materials()[materialIndex]),bindings,mainSets);
		if(!sets[setIndex]) // 최종 일반 자원 세트 생성 실패는 false로 전달한다.
		{
			return false;
		}
	}
	return true;
}
}
