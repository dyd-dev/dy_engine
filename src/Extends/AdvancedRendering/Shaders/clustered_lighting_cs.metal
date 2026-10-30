#include <metal_stdlib>
using namespace metal;
struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
struct ClusterParameters {float4x4 view;float4x4 projection;float4x4 inverseProjection;float4 depth;uint4 dimensions;uint4 counts;};
inline bool ClusterSpotOutside(ClusterLight light,float3 cellCenter,float3 cellExtent,float3 lightCenter,
    constant ClusterParameters& clusters)
{
    float3 source=light.positionRange.xyz,direction=light.directionOuterCos.xyz;
    float range=light.positionRange.w,outerCos=light.directionOuterCos.w;
    float3x3 viewLinear(clusters.view[0].xyz,clusters.view[1].xyz,clusters.view[2].xyz);
    if(!all(isfinite(source)) || !all(isfinite(direction)) || !all(isfinite(cellCenter)) ||
        !all(isfinite(cellExtent)) || !all(isfinite(lightCenter)) ||
        !all(isfinite(viewLinear[0])) || !all(isfinite(viewLinear[1])) || !all(isfinite(viewLinear[2])) ||
        !all(isfinite(clusters.view[3].xyz)) || !isfinite(range) || !isfinite(outerCos) || range<=0 || outerCos<=0 || any(cellExtent<0.f))return false;
    float directionSquared=dot(direction,direction);
    if(!isfinite(directionSquared) || directionSquared<=1e-12f)return false;
    float directionLength=sqrt(directionSquared);
    direction/=directionLength;
    // Match the fragment direction length, then widen the floating-point boundary.
    float effectiveCos=outerCos/directionLength;
    if(!isfinite(effectiveCos))return false;
    float cosine=clamp(effectiveCos-1e-5f,0.f,1.f);
    float sine=sqrt(max(1.f-cosine*cosine,0.f));
    float3 axis=viewLinear*direction,offset=cellCenter-lightCenter;
    float axisSquared=dot(axis,axis);
    if(!all(isfinite(offset)) || !isfinite(axisSquared) || axisSquared<=1e-12f)return false;
    axis/=sqrt(axisSquared);
    float3 radial=offset-axis*dot(offset,axis);
    float radialSquared=dot(radial,radial);
    if(!isfinite(radialSquared) || radialSquared<=1e-12f)return false;
    // Any plane is valid: the support below accounts for the complete affine view.
    float3 plane=cosine*(radial/sqrt(radialSquared))-sine*axis;
    float3 worldPlane=transpose(viewLinear)*plane;
    float planeSquared=dot(worldPlane,worldPlane);
    if(!all(isfinite(plane)) || !all(isfinite(worldPlane)) || !isfinite(planeSquared) || planeSquared<=1e-12f)return false;
    float planeLength=sqrt(planeSquared),along=dot(worldPlane,direction);
    // Explicit perpendicular avoids cancellation between squared lengths.
    float3 perpendicular=worldPlane-along*direction;
    float across=length(perpendicular);
    float support=along>=cosine*planeLength ? planeLength : cosine*along+sine*across;
    float maximum=range*max(support,0.f);
    float minimum=dot(plane,offset)-dot(abs(plane),cellExtent);
    float sourceMagnitude=max(max(abs(source.x),abs(source.y)),abs(source.z))+range;
    float linearMagnitude=dot(abs(plane),abs(viewLinear[0])+abs(viewLinear[1])+abs(viewLinear[2]));
    float errorScale=dot(abs(plane),abs(cellCenter)+cellExtent+abs(clusters.view[3].xyz))+
        sourceMagnitude*linearMagnitude;
    if(!isfinite(along) || !isfinite(across) || !isfinite(support) || !isfinite(errorScale))return false;
    float margin=1e-4f*max(1.f,errorScale);
    if(!isfinite(maximum) || !isfinite(minimum) || !isfinite(margin))return false;
    return minimum>maximum+margin;
}
kernel void clusteredLightingCompute(const device ClusterLight* clusterLights [[buffer(19)]],
    device uint* clusterList [[buffer(20)]],constant ClusterParameters& clusters [[buffer(21)]],
    uint cell [[thread_position_in_grid]],uint lane [[thread_index_in_threadgroup]])
{
    threadgroup ClusterLight lightBatch[64];
    threadgroup float4 lightBounds[64];
    uint total=clusters.dimensions.x*clusters.dimensions.y*clusters.dimensions.z;
    bool validCell=cell<total,overflow=false;
    float3 minimum=float3(1e30f),maximum=float3(-1e30f);
    if(validCell)
    {
        uint3 index=uint3(cell%clusters.dimensions.x,(cell/clusters.dimensions.x)%clusters.dimensions.y,
            cell/(clusters.dimensions.x*clusters.dimensions.y));
        float2 lower=float2(index.xy)/float2(clusters.dimensions.xy)*2-1;
        float2 upper=float2(index.xy+uint2(1))/float2(clusters.dimensions.xy)*2-1;
        float begin=float(index.z)/float(clusters.dimensions.z),end=float(index.z+1)/float(clusters.dimensions.z);
        float nearDepth=clusters.counts.z!=0?clusters.depth.x*exp(clusters.depth.z*begin):mix(clusters.depth.x,clusters.depth.y,begin);
        float farDepth=clusters.counts.z!=0?clusters.depth.x*exp(clusters.depth.z*end):mix(clusters.depth.x,clusters.depth.y,end);
        for(uint corner=0;corner<8;++corner)
        {
            float2 xy=float2((corner&1u)!=0?upper.x:lower.x,(corner&2u)!=0?upper.y:lower.y);
            float4 projected=clusters.inverseProjection*float4(xy,0,1);
            float3 point=projected.xyz/projected.w;float distance=(corner&4u)!=0?farDepth:nearDepth;
            if(clusters.counts.z!=0)point*=distance/max(-point.z,1e-20f);else point.z=-distance;
            minimum=min(minimum,point);maximum=max(maximum,point);
        }
    }
    float3 cellCenter=(minimum+maximum)*.5f,cellExtent=(maximum-minimum)*.5f;
    uint base=validCell?cell*(clusters.dimensions.w+1u):0u,count=0,first=0;
    while(first<clusters.counts.x)
    {
        uint batchCount=min(64u,clusters.counts.x-first);
        if(lane<batchCount)
        {
            ClusterLight light=clusterLights[first+lane];
            lightBatch[lane]=light;
            lightBounds[lane]=float4((clusters.view*float4(light.positionRange.xyz,1)).xyz,
                light.positionRange.w*clusters.depth.w);
        }
        // Inactive/overflow cells still load their lane and reach both barriers.
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if(validCell && !overflow)
            for(uint local=0;local<batchCount && !overflow;++local)
            {
                ClusterLight light=lightBatch[local];
                float3 center=lightBounds[local].xyz;
                float radius=lightBounds[local].w;
                float3 delta=center-clamp(center,minimum,maximum);
                if(dot(delta,delta)<=radius*radius)
                {
                    if(light.coneShadow.y==3.f && ClusterSpotOutside(light,cellCenter,cellExtent,center,clusters))continue;
                    // Overflow retains the complete-light-list fallback.
                    if(count==clusters.dimensions.w)overflow=true;
                    else clusterList[base+1u+count++]=first+local;
                }
            }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        first+=batchCount;
    }
    if(validCell)clusterList[base]=overflow?0xffffffffu:count;
}
