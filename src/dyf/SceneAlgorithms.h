#pragma once
#include "dyf/Camera.h"
#include "dyf/Material.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <iterator>
#include <functional>
#include <vector>

namespace dyf
{
    // 재질 ID와 이미지 저장소를 제외한 실제 셰이더·파이프라인 상수의 동등성이다.
    inline bool EqualMaterial(const MaterialDesc& a,const MaterialDesc& b)
    {
        return a.baseColor.x==b.baseColor.x && a.baseColor.y==b.baseColor.y && a.baseColor.z==b.baseColor.z && a.baseColor.w==b.baseColor.w &&
            a.emissiveColor.x==b.emissiveColor.x && a.emissiveColor.y==b.emissiveColor.y && a.emissiveColor.z==b.emissiveColor.z &&
            a.metallicFactor==b.metallicFactor && a.roughnessFactor==b.roughnessFactor && a.normalScale==b.normalScale && a.occlusionStrength==b.occlusionStrength &&
            a.normalYSign==b.normalYSign && a.transmissionFactor==b.transmissionFactor && a.indexOfRefraction==b.indexOfRefraction &&
            a.thickness==b.thickness && a.attenuationColor.x==b.attenuationColor.x && a.attenuationColor.y==b.attenuationColor.y &&
            a.attenuationColor.z==b.attenuationColor.z && a.attenuationDistance==b.attenuationDistance &&
            a.alphaMode==b.alphaMode && a.alphaCutoff==b.alphaCutoff && a.doubleSided==b.doubleSided && a.unlit==b.unlit;
    }
    inline size_t MaterialScalarHash(const MaterialDesc& material)
    {
        size_t seed=0;
        const auto combine=[&](size_t value) {seed^=value+size_t{0x9e3779b9}+(seed<<6)+(seed>>2);};
        const float values[]={material.baseColor.x,material.baseColor.y,material.baseColor.z,material.baseColor.w,
            material.emissiveColor.x,material.emissiveColor.y,material.emissiveColor.z,
            material.metallicFactor,material.roughnessFactor,material.normalScale,material.occlusionStrength,
            material.normalYSign,material.transmissionFactor,material.indexOfRefraction,material.thickness,
            material.attenuationColor.x,material.attenuationColor.y,material.attenuationColor.z,
            material.attenuationDistance,material.alphaCutoff};
        // float 동등 비교와 맞춰 +0과 -0도 같은 해시를 사용하며 패딩은 읽지 않는다.
        for(float value:values)combine(std::hash<float>{}(value));
        combine(std::hash<uint32_t>{}(static_cast<uint32_t>(material.alphaMode)));
        combine(std::hash<bool>{}(material.doubleSided));
        combine(std::hash<bool>{}(material.unlit));
        return seed;
    }

    inline bool OutsideCamera(const Math::Bounds3& bounds,const Math::float4x4& world,const Camera& camera)
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
            bool canReject=false;
            for(uint32_t plane=0;plane<6;++plane) {
                outside[plane]=outside[plane] && distances[plane]<-epsilon;
                canReject=canReject || outside[plane];
            }
            // Each flag can only change from true to false. Once every plane has
            // an inside corner, later corners cannot make this box rejectable.
            if(!canReject)return false;
        }
        return std::any_of(std::begin(outside),std::end(outside),[](bool value){return value;});
    }

    struct SpotConeAngles { float inner,outer; };
    // Both lighting and shadow projection consume the same sorted half-angles.
    // Non-finite inputs have no useful cone; treat each as zero before sorting.
    inline SpotConeAngles EffectiveSpotCone(float inner,float outer)
    {
        if(!std::isfinite(inner)) inner=0;
        if(!std::isfinite(outer)) outer=0;
        constexpr float maximum=1.55334306f; // 89 degrees
        return {std::clamp(std::min(inner,outer),0.f,maximum),
            std::clamp(std::max(inner,outer),0.f,maximum)};
    }
// 내장 셰이더의 배열 크기에 맞춰 enabled/priority 순서로 광원을 선택한다.
// 이 상한을 넘는 광원은 현재 렌더링에서 제외된다.
	template <typename LightType>
	[[nodiscard]] inline std::vector<uint32_t> SelectActiveLightIndices(
		const std::vector<LightType>& lights,
		uint32_t capacity)
	{
		std::vector<uint32_t> indices;
		indices.reserve(lights.size());
		for(uint32_t index = 0u; index < static_cast<uint32_t>(lights.size()); ++index)
		{
			if(lights[index].enabled) indices.push_back(index);
		}
		std::stable_sort(indices.begin(), indices.end(), [&lights](uint32_t left, uint32_t right)
		{
			return lights[left].priority > lights[right].priority;
		});
		if(indices.size() > capacity) indices.resize(capacity);
		return indices;
	}

}
