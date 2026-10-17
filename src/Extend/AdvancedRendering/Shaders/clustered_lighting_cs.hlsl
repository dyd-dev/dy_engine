struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
StructuredBuffer<ClusterLight> clusterLights:register(t19);
RWStructuredBuffer<uint> clusterList:register(u20);
cbuffer Parameters:register(b21) {column_major float4x4 clusterView;column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;float4 clusterDepth;uint4 clusterDimensions;uint4 clusterCounts;};
[numthreads(64,1,1)]
void main(uint3 dispatch:SV_DispatchThreadID)
{
    uint cell=dispatch.x,total=clusterDimensions.x*clusterDimensions.y*clusterDimensions.z;
    if(cell>=total)return;
    uint3 index=uint3(cell%clusterDimensions.x,(cell/clusterDimensions.x)%clusterDimensions.y,
        cell/(clusterDimensions.x*clusterDimensions.y));
    float2 lower=float2(index.xy)/float2(clusterDimensions.xy)*2-1;
    float2 upper=float2(index.xy+uint2(1,1))/float2(clusterDimensions.xy)*2-1;
    float begin=float(index.z)/float(clusterDimensions.z),end=float(index.z+1)/float(clusterDimensions.z);
    float nearDepth=clusterCounts.z!=0?clusterDepth.x*exp(clusterDepth.z*begin):lerp(clusterDepth.x,clusterDepth.y,begin);
    float farDepth=clusterCounts.z!=0?clusterDepth.x*exp(clusterDepth.z*end):lerp(clusterDepth.x,clusterDepth.y,end);
    float3 minimum=float3(1e30,1e30,1e30),maximum=-minimum;
    for(uint corner=0;corner<8;++corner)
    {
        float2 xy=float2((corner&1)!=0?upper.x:lower.x,(corner&2)!=0?upper.y:lower.y);
        float4 projected=mul(clusterInverseProjection,float4(xy,0,1));
        float3 point=projected.xyz/projected.w;float distance=(corner&4)!=0?farDepth:nearDepth;
        if(clusterCounts.z!=0)point*=distance/max(-point.z,1e-20);else point.z=-distance;
        minimum=min(minimum,point);maximum=max(maximum,point);
    }
    uint base=cell*(clusterDimensions.w+1),count=0;
    for(uint lightIndex=0;lightIndex<clusterCounts.x;++lightIndex)
    {
        ClusterLight light=clusterLights[lightIndex];
        float3 center=mul(clusterView,float4(light.positionRange.xyz,1)).xyz;
        float radius=light.positionRange.w*clusterDepth.w;
        float3 delta=center-clamp(center,minimum,maximum);
        if(dot(delta,delta)<=radius*radius)
        {
            // 용량 초과 표식은 전체 광원 순회로 전환한다.
            if(count==clusterDimensions.w){clusterList[base]=0xffffffffu;return;}
            clusterList[base+1+count++]=lightIndex;
        }
    }
    clusterList[base]=count;
}
