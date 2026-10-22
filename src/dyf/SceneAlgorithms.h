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
    // 재질의 셰이더 상수와 파이프라인 설정을 비교하며, ID와 이미지 정체성은 별도 키에서 비교한다.
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
    // EqualMaterial이 비교하는 스칼라·플래그를 해시한다. 해시 일치는 후보 축소용이고 실제 동등 비교를 대신하지 않는다.
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

    // 로컬 좌표의 각 축에 나란한 면으로 물체를 감싸는 상자(AABB)를 사용한다.
    // 여덟 꼭짓점을 world와 view-projection으로 변환하고, 같은 clip 평면 바깥에 모든 꼭짓점이 놓이는지 검사한다.
    inline bool OutsideCamera(const Math::Bounds3& bounds,const Math::float4x4& world,const Camera& camera)
    {
        if(!bounds.valid)return false; // 경계가 없으면 화면 밖이라고 판단하지 않는다.
        // Unknown projective world/view contracts fail open. Standard perspective and
        // orthographic projections (including reversed axes) use homogeneous ZO planes.
        const auto affine=[](const Math::float4x4& m) {return m.m[3]==0 && m.m[7]==0 && m.m[11]==0 && m.m[15]==1;};
        const auto& p=camera.projection;
        if(!affine(world) || !affine(camera.view) || // 지원하지 않는 world/view/투영 형태이면 컬링하지 않는다.
            !((p.m[11]==-1 && p.m[15]==0) || (p.m[11]==0 && p.m[15]==1)) ||
            p.m[1]!=0 || p.m[2]!=0 || p.m[3]!=0 || p.m[4]!=0 || p.m[6]!=0 || p.m[7]!=0 ||
            p.m[0]==0 || p.m[5]==0 || p.m[10]==0)return false;
        const auto viewProjection=p*camera.view;
        for(float v:viewProjection.m)if(!std::isfinite(v))return false; // 카메라 변환 결과가 비유한 값이면 객체를 남긴다.
        for(float v:world.m)if(!std::isfinite(v))return false; // world 행렬에 비유한 값이 있으면 객체를 남긴다.
        // 모든 꼭짓점이 같은 평면 바깥에 있을 때만 AABB 전체를 제외한다. 꼭짓점 하나라도 안쪽 또는 경계 여유 안에 있으면 그 평면으로는 제외할 수 없다.
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
            // 큰 양수 항과 음수 항이 더해지면 최종 clip 값이 작아도 계산 오차는 클 수 있다.
            // 각 곱셈 항의 절댓값 합까지 모아 경계 여유를 정하므로 큰 월드 좌표의 객체를 반올림 오차만으로 제거하지 않는다.
            double errorScale=1;
            for(uint32_t row=0;row<4;++row) {
                double sum=0;
                for(uint32_t column=0;column<4;++column) {
                    const double coefficient=viewProjection.m[column*4+row];
                    clip[row]+=coefficient*transformed[column];sum+=std::abs(coefficient)*magnitude[column];
                }
                errorScale=std::max(errorScale,sum);
            }
            for(double v:clip)if(!std::isfinite(v))return false; // 꼭짓점의 clip 변환에 NaN·무한대가 생기면 화면 밖이라고 확정하지 않고 객체를 남긴다.
            // Includes cancellation at large world coordinates, not just final clip size.
            const double epsilon=1e-5*errorScale;
            // 좌우·상하 평면은 w±x/y, 깊이 0..w 범위는 z와 w-z로 검사한다.
            const double distances[]={clip[3]+clip[0],clip[3]-clip[0],clip[3]+clip[1],clip[3]-clip[1],clip[2],clip[3]-clip[2]};
            bool canReject=false;
            for(uint32_t plane=0;plane<6;++plane) {
                outside[plane]=outside[plane] && distances[plane]<-epsilon;
                canReject=canReject || outside[plane];
            }
            if(!canReject)return false; // 각 평면에 대해 안쪽 또는 경계 여유 안의 꼭짓점을 이미 찾았다면, 남은 꼭짓점을 검사해도 전체가 바깥이 될 수 없어 객체를 남긴다.
        }
        return std::any_of(std::begin(outside),std::end(outside),[](bool value){return value;});
    }

    struct SpotConeAngles { float inner,outer; };
    // 조명과 그림자에 같은 반각을 쓰도록 비유한 입력을 0으로 바꾸고, 작은 값·큰 값 순서로 정렬해 0..89도로 제한한다.
    inline SpotConeAngles EffectiveSpotCone(float inner,float outer)
    {
        if(!std::isfinite(inner)) inner=0; // 안쪽 각도가 비유한 값이면 0으로 바꾼다.
        if(!std::isfinite(outer)) outer=0; // 바깥쪽 각도가 비유한 값이면 0으로 바꾼다.
        constexpr float maximum=1.55334306f; // 89 degrees
        return {std::clamp(std::min(inner,outer),0.f,maximum),
            std::clamp(std::max(inner,outer),0.f,maximum)};
    }
	template <typename LightType>
	// 켜진 광원의 인덱스만 모아 우선순위가 높은 순으로 안정 정렬하고 셰이더 용량만큼 남긴다. 같은 우선순위는 원래 순서다.
	[[nodiscard]] inline std::vector<uint32_t> SelectActiveLightIndices(
		const std::vector<LightType>& lights,
		uint32_t capacity)
	{
		std::vector<uint32_t> indices;
		indices.reserve(lights.size());
		for(uint32_t index = 0u; index < static_cast<uint32_t>(lights.size()); ++index)
		{
			if(lights[index].enabled) indices.push_back(index); // 활성화한 광원만 선택 후보에 넣는다.
		}
		std::stable_sort(indices.begin(), indices.end(), [&lights](uint32_t left, uint32_t right)
		{
			return lights[left].priority > lights[right].priority;
		});
		if(indices.size() > capacity) indices.resize(capacity); // 용량을 넘으면 낮은 우선순위의 뒤쪽 후보를 제외한다.
		return indices;
	}

}
