#pragma once
#include "dyf/Camera.h"
#include "dyf/Material.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <iterator>
#include <functional>
#include <vector>

namespace dyf
{
    // Optical transmission and geometric alpha coverage are independent. Both
    // need the transparent lighting path, while retaining their authored alpha mode.
    inline bool UsesBlendedRendering(const MaterialDesc& material)
    {
        return material.alphaMode == MaterialAlphaMode::Blend || material.transmissionFactor > 0.0f;
    }

    inline bool IsAffineTransform(const Math::float4x4& matrix)
    {
        if(matrix.m[3]!=0 || matrix.m[7]!=0 || matrix.m[11]!=0 || matrix.m[15]!=1)return false;
        for(float value:matrix.m)if(!std::isfinite(value))return false;
        return true;
    }

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

    // 로컬 AABB를 카메라 평면의 지지점으로 검사한다. 카메라 평면은 재사용하고 지원하지 않는 투영은 컬링하지 않는다.
    class CameraFrustum
    {
        Math::float4x4 m_viewProjection={};
        std::array<std::array<double,4>,6> m_planes={};
        bool m_valid=false;
    public:
        explicit CameraFrustum(const Camera& camera)
        {
            // Unknown projection contracts fail open, including oblique/projective views.
            const auto& p=camera.projection;
            if(!IsAffineTransform(camera.view) ||
                !((p.m[11]==-1 && p.m[15]==0) || (p.m[11]==0 && p.m[15]==1)) ||
                p.m[1]!=0 || p.m[2]!=0 || p.m[3]!=0 || p.m[4]!=0 || p.m[6]!=0 || p.m[7]!=0 ||
                p.m[0]==0 || p.m[5]==0 || p.m[10]==0)return;
            m_viewProjection=p*camera.view;
            for(float value:m_viewProjection.m)if(!std::isfinite(value))return;
            for(uint32_t plane=0;plane<6;++plane)
                for(uint32_t column=0;column<4;++column)
                {
                    const double a=m_viewProjection.m[column*4+3];
                    const double b=m_viewProjection.m[column*4+plane/2];
                    m_planes[plane][column]=plane==4 ? b : a+(plane%2 ? -b : b);
                }
            m_valid=true;
        }
        bool Outside(const Math::Bounds3& bounds,const Math::float4x4& world) const
        {
            if(!m_valid || !bounds.valid || !IsAffineTransform(world))return false;
            const double minimum[]={bounds.min.x,bounds.min.y,bounds.min.z};
            const double maximum[]={bounds.max.x,bounds.max.y,bounds.max.z};
            double magnitude[4]={};
            for(uint32_t axis=0;axis<3;++axis)
                if(!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) || minimum[axis]>maximum[axis])return false;
            double center[4]={0,0,0,1},clip[4]={};
            for(uint32_t row=0;row<3;++row)
            {
                center[row]=world.m[12+row];
                for(uint32_t column=0;column<3;++column)
                    center[row]+=world.m[column*4+row]*(minimum[column]+maximum[column])*.5;
            }
            for(uint32_t row=0;row<4;++row)
                for(uint32_t column=0;column<4;++column)
                    clip[row]+=m_viewProjection.m[column*4+row]*center[column];
            // An interior point proves intersection. Most visible objects stop before support tests.
            if(clip[2]>=0 && clip[3]>=clip[2] && clip[3]>=std::abs(clip[0]) && clip[3]>=std::abs(clip[1]))return false;
            for(uint32_t row=0;row<4;++row)
            {
                magnitude[row]=std::abs(double(world.m[12+row]));
                for(uint32_t column=0;column<3;++column)
                    magnitude[row]+=std::abs(double(world.m[column*4+row]))*
                        std::max(std::abs(minimum[column]),std::abs(maximum[column]));
            }
            // This bound covers the cancellation scale of every corner in the old test.
            double errorScale=1;
            for(uint32_t row=0;row<4;++row)
            {
                double sum=0;
                for(uint32_t column=0;column<4;++column)
                    sum+=std::abs(double(m_viewProjection.m[column*4+row]))*magnitude[column];
                errorScale=std::max(errorScale,sum);
            }
            // 큰 월드 좌표에서 상쇄되는 항의 크기도 오차 여유에 포함해 반올림만으로 객체를 제거하지 않는다.
            const double epsilon=1e-5*errorScale;
            for(const auto& plane:m_planes)
            {
                // Pull the world-space plane into local space and test its support vertex.
                double distance=plane[3];
                for(uint32_t row=0;row<3;++row)distance+=plane[row]*world.m[12+row];
                for(uint32_t axis=0;axis<3;++axis)
                {
                    double coefficient=0;
                    for(uint32_t row=0;row<3;++row)coefficient+=plane[row]*world.m[axis*4+row];
                    distance+=coefficient*(coefficient>=0 ? maximum[axis] : minimum[axis]);
                }
                if(distance < -epsilon)return true;
            }
            return false;
        }
    };

    inline bool OutsideCamera(const Math::Bounds3& bounds,const Math::float4x4& world,const Camera& camera)
    {
        return CameraFrustum(camera).Outside(bounds,world);
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
