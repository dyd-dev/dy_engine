#include "RendererInternal.h"
#include "dyf/Scene.h"
#include <algorithm>
#include <cstring>

namespace dyf {
bool Renderer::Impl::PrepareGpuShadowPass(const Scene& scene,const ShadowData& shadows,RHI::ICommandList& uploads)
{
    auto& state=gpuShadows;const auto& casters=shadowCasterScratch;
    state.coverageScratch.resize(casters.size());
    for(size_t i=0;i<casters.size();++i) {
        const auto& caster=casters[i];const auto& material=scene.Materials()[caster.material];
        auto& coverage=state.coverageScratch[i];coverage={caster.coverageFlags,0,0};
        if(material.alphaMode==MaterialAlphaMode::Mask) {
            std::memcpy(&coverage[1],&material.alphaCutoff,sizeof(uint32_t));
            std::memcpy(&coverage[2],&material.baseColor.w,sizeof(uint32_t));
        }
    }
    const auto sameBounds=[](const Math::Bounds3& a,const Math::Bounds3& b) {
        return a.valid==b.valid && (!a.valid || (a.min.x==b.min.x && a.min.y==b.min.y && a.min.z==b.min.z &&
            a.max.x==b.max.x && a.max.y==b.max.y && a.max.z==b.max.z));
    };
    bool same=state.generation==shadowArenaGeneration && state.casters.size()==casters.size() &&
        state.coverage==state.coverageScratch;
    for(size_t i=0;same && i<casters.size();++i) {
        const auto& a=casters[i];const auto& b=state.casters[i];
        same=a.entity==b.entity && a.mesh==b.mesh && a.material==b.material && a.pipeline==b.pipeline &&
            a.set==b.set && a.vertices==b.vertices && a.indices==b.indices && a.indexFormat==b.indexFormat &&
            a.vertexOffset==b.vertexOffset && a.firstIndex==b.firstIndex && a.indexCount==b.indexCount &&
            a.clustered==b.clustered && a.arena==b.arena && sameBounds(a.localBounds,b.localBounds);
    }
    if(!same) {
        using Key=std::tuple<uintptr_t,uintptr_t,uintptr_t,uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t>;
        struct Item {Key key;GpuShadowVisibility::Input input;uint32_t caster,firstIndex,indexCount;};
        std::vector<Item> items;
        for(uint32_t index=0;index<casters.size();++index) {
            const auto& caster=casters[index];const auto& mesh=m_meshes[caster.mesh];
            const auto& material=scene.Materials()[caster.material];
            const uint32_t baseIndex=caster.arena ? mesh.shadowArena.indexByteOffset/
                (caster.indexFormat==RHI::Format::R16_UINT?2u:4u) : 0;
            const auto append=[&](uint32_t first,uint32_t count,const Math::Bounds3& bounds) {
                Item item;
                item.key={reinterpret_cast<uintptr_t>(caster.pipeline),reinterpret_cast<uintptr_t>(caster.set),
                    reinterpret_cast<uintptr_t>(caster.vertices),reinterpret_cast<uintptr_t>(caster.indices),
                    static_cast<uint32_t>(caster.indexFormat),material.alphaMode==MaterialAlphaMode::Mask?caster.material:0,
                    first+baseIndex,count,caster.vertexOffset};
                item.input.minimum={bounds.min.x,bounds.min.y,bounds.min.z,bounds.valid?1.f:0.f};
                item.input.maximum={bounds.max.x,bounds.max.y,bounds.max.z,0};
                item.input.ids={caster.entity,0,0,0};
                item.caster=index;item.firstIndex=first+baseIndex;item.indexCount=count;
                items.push_back(item);
            };
            if(caster.clustered && !mesh.shadowArena.clusters.empty())
                for(const auto& cluster:mesh.shadowArena.clusters)append(cluster.firstIndex,cluster.indexCount,cluster.bounds);
            else append(caster.firstIndex,caster.indexCount,caster.localBounds);
        }
        if(items.size()>UINT32_MAX/sizeof(GpuShadowVisibility::Instance)/shadows.viewCount)return false;
        // Batch topology is stable across camera/transform motion; rebuild it only after metadata edits.
        std::sort(items.begin(),items.end(),[](const Item& a,const Item& b) {
            return a.key!=b.key?a.key<b.key:a.caster<b.caster;
        });
        state.inputs.clear();state.batches.clear();state.representatives.clear();
        state.inputs.reserve(items.size());
        uint32_t capacity=0;
        for(size_t first=0;first<items.size();) {
            size_t end=first+1;while(end<items.size() && items[end].key==items[first].key)++end;
            const auto& item=items[first];const auto& caster=casters[item.caster];
            const uint32_t batch=static_cast<uint32_t>(state.batches.size()),count=static_cast<uint32_t>(end-first);
            state.batches.push_back({item.indexCount,item.firstIndex,caster.vertexOffset,capacity,count,{}});
            state.representatives.push_back(item.caster);
            for(size_t i=first;i<end;++i) {
                auto input=items[i].input;input.ids[1]=batch;state.inputs.push_back(input);
            }
            capacity+=count;first=end;
        }
        state.casters=casters;state.coverage.swap(state.coverageScratch);state.generation=shadowArenaGeneration;
    }
    state.views.assign(shadows.viewCount,{});
    for(uint32_t i=0;i<shadows.viewCount;++i) {
        auto& view=state.views[i];const auto& volume=shadows.casterVolumes[i];
        view.projection=shadows.constants.lightViewProjectionMatrix[i];
        view.info={volume.count,shadows.dirtyViews[i]?1u:0u,0,0};
        for(uint32_t plane=0;plane<volume.count;++plane) {
            const auto& p=volume.planes[plane];
            view.receiverPlanes[plane]={float(p[0]),float(p[1]),float(p[2]),float(p[3])};
        }
    }
    if(!state.visibility)state.visibility=std::make_unique<GpuShadowVisibility>(*device);
    if(!state.visibility->Prepare(state.inputs,state.batches,state.views,instanceBuffer) ||
        !state.visibility->RecordUploads(uploads))return false;
    state.active=true;return true;
}
}
