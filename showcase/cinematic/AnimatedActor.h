#pragma once

#include "dyf/Extends/Model/ModelScene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace showcase
{
class AnimatedActor
{
public:
    bool Load(dyf::ModelScene& scene,dyf::ModelSceneDesc description,uint32_t clip,
        dyf::Math::float3 eye,dyf::Math::float3 target,bool positioned)
    {
        using namespace dyf;
        if(description.path.empty()) return true;
        const uint32_t firstEntity=scene.GetEntityCount();
        auto feet=description.position;
        if(!positioned)
        {
            const auto forward=Math::NormalizeOr({target.x-eye.x,target.y-eye.y,0},{0,1,0});
            feet=eye+forward*std::max(4.f,description.normalizedSize*2.f);
            feet.z=eye.z-1.6f;
            float nearest=std::numeric_limits<float>::infinity();
            // 시작 구도 앞쪽의 실제 불투명 표면에 한 번만 수직으로 배치한다.
            const Math::float3 origin{feet.x,feet.y,eye.z};
            for(uint32_t entityIndex=0;entityIndex<firstEntity;++entityIndex)
            {
                const auto entity=static_cast<EntityID>(entityIndex);
                if(scene.GetEntity(entity).GetMaterial().alphaMode!=MaterialAlphaMode::Opaque) continue;
                const auto mesh=ToIndex(scene.GetEntityMesh(entity));
                if(mesh>=scene.Meshes().size() || !scene.Meshes()[mesh]) continue;
                Math::float4x4 inverse;
                if(!Math::Inverse(scene.GetTransform(entity),inverse)) continue;
                const auto rayOrigin=Math::TransformPoint(inverse,origin);
                const auto rayDirection=Math::TransformVector(inverse,{0,0,-1});
                const auto& geometry=*scene.Meshes()[mesh];
                for(size_t i=0;i+2<geometry.indices.size();i+=3)
                {
                    const auto a=geometry.vertices[geometry.indices[i]].position;
                    const auto e1=geometry.vertices[geometry.indices[i+1]].position-a;
                    const auto e2=geometry.vertices[geometry.indices[i+2]].position-a;
                    const auto p=Math::Cross(rayDirection,e2);const float determinant=Math::Dot(e1,p);
                    if(std::abs(determinant)<1e-8f) continue;
                    const auto relative=rayOrigin-a;const float u=Math::Dot(relative,p)/determinant;
                    if(u<0 || u>1) continue;
                    const auto q=Math::Cross(relative,e1);const float v=Math::Dot(rayDirection,q)/determinant;
                    if(v<0 || u+v>1) continue;
                    const float distance=Math::Dot(e2,q)/determinant;
                    if(distance>=0 && distance<nearest) nearest=distance;
                }
            }
            if(std::isfinite(nearest)) feet.z=origin.z-nearest;
            else std::fputs("Actor: no supporting surface below the opening view; use --actor-position X Y Z to place its feet.\n",stderr);
        }
        description.position=feet;
        std::fprintf(stderr,"Loading animated actor: %s\n",description.path.c_str());
        if(!AddModelToScene(scene,description,&m_instance)) return false;
        auto* instance=scene.TryGetModelInstance(m_instance);
        const auto* asset=instance?scene.TryGetModelAsset(instance->assetId):nullptr;
        if(!asset || clip>=asset->animations.size() || !std::isfinite(asset->animations[clip].duration) ||
            asset->animations[clip].duration<=0)
        {
            std::fprintf(stderr,"Actor requires an animated model and a valid --actor-clip index (requested %u).\n",clip);
            return false;
        }
        m_duration=asset->animations[clip].duration;
        if(!scene.PlayAnimation(m_instance,clip,true) || !scene.SetAnimationPaused(m_instance,true)) return false;
        Math::Bounds3 bounds;
        for(uint32_t i=firstEntity;i<scene.GetEntityCount();++i)
        {
            const auto entity=static_cast<EntityID>(i);const auto mesh=ToIndex(scene.GetEntityMesh(entity));
            if(mesh>=scene.Meshes().size() || !scene.Meshes()[mesh]) continue;
            for(const auto& vertex:scene.Meshes()[mesh]->vertices)
                bounds.Include(Math::TransformPoint(scene.GetTransform(entity),vertex.position));
        }
        if(!bounds.valid) return false;
        instance->rootTransform=Math::Translation({0,0,feet.z-bounds.min.z})*instance->rootTransform;
        std::fprintf(stderr,"Actor clip %u: %s | %.3f s loop | feet=(%.3f %.3f %.3f), normalized size %.3f. Scene time controls animation.\n",
            clip,asset->animations[clip].name.c_str(),m_duration,feet.x,feet.y,feet.z,description.normalizedSize);
        return Update(scene,0);
    }

    bool Update(dyf::ModelScene& scene,double sceneSeconds)
    {
        if(!Loaded()) return true;
        auto* instance=scene.TryGetModelInstance(m_instance);
        if(!instance) return false;
        // 누적 델타 대신 장면의 절대 시간을 사용해 탐색·재시작·A/B 포즈를 일치시킨다.
        instance->playback.time=float(std::fmod(std::max(0.0,sceneSeconds),double(m_duration)));
        return scene.UpdateAnimations(0);
    }

    bool Loaded() const {return dyf::IsValid(m_instance);}

private:
    dyf::ModelInstanceID m_instance=dyf::ModelInstanceID::Invalid;
    float m_duration=0;
};
}
