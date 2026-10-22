struct ClusterLight {float4 positionRange;float4 colorIntensity;float4 directionOuterCos;float4 coneShadow;};
StructuredBuffer<ClusterLight> clusterLights:register(t19);
RWStructuredBuffer<uint> clusterList:register(u20);
cbuffer Parameters:register(b21) {column_major float4x4 clusterView;column_major float4x4 clusterProjection;
    column_major float4x4 clusterInverseProjection;float4 clusterDepth;uint4 clusterDimensions;uint4 clusterCounts;};
bool ClusterSpotOutside(ClusterLight light,float3 cellCenter,float3 cellExtent,float3 lightCenter)
{
    float3 source=light.positionRange.xyz,direction=light.directionOuterCos.xyz;
    float range=light.positionRange.w,outerCos=light.directionOuterCos.w;
    float3x3 viewLinear=(float3x3)clusterView;
    float3 column0=float3(viewLinear[0][0],viewLinear[1][0],viewLinear[2][0]);
    float3 column1=float3(viewLinear[0][1],viewLinear[1][1],viewLinear[2][1]);
    float3 column2=float3(viewLinear[0][2],viewLinear[1][2],viewLinear[2][2]);
    float3 translation=float3(clusterView[0][3],clusterView[1][3],clusterView[2][3]);
    if(!all(isfinite(source)) || !all(isfinite(direction)) || !all(isfinite(cellCenter)) ||
        !all(isfinite(cellExtent)) || !all(isfinite(lightCenter)) ||
        !all(isfinite(column0)) || !all(isfinite(column1)) || !all(isfinite(column2)) ||
        !all(isfinite(translation)) || !isfinite(range) || !isfinite(outerCos) || range<=0 || outerCos<=0 || any(cellExtent<0))return false;
    float directionSquared=dot(direction,direction);
    if(!isfinite(directionSquared) || directionSquared<=1e-12)return false;
    float directionLength=sqrt(directionSquared);
    direction/=directionLength;
    // Match the fragment direction length, then widen the floating-point boundary.
    float effectiveCos=outerCos/directionLength;
    if(!isfinite(effectiveCos))return false;
    float cosine=clamp(effectiveCos-1e-5,0.0,1.0);
    float sine=sqrt(max(1.0-cosine*cosine,0.0));
    float3 axis=mul(viewLinear,direction),offset=cellCenter-lightCenter;
    float axisSquared=dot(axis,axis);
    if(!all(isfinite(offset)) || !isfinite(axisSquared) || axisSquared<=1e-12)return false;
    axis/=sqrt(axisSquared);
    float3 radial=offset-axis*dot(offset,axis);
    float radialSquared=dot(radial,radial);
    if(!isfinite(radialSquared) || radialSquared<=1e-12)return false;
    // Any plane is valid: the support below accounts for the complete affine view.
    float3 plane=cosine*(radial/sqrt(radialSquared))-sine*axis;
    float3 worldPlane=mul(transpose(viewLinear),plane);
    float planeSquared=dot(worldPlane,worldPlane);
    if(!all(isfinite(plane)) || !all(isfinite(worldPlane)) || !isfinite(planeSquared) || planeSquared<=1e-12)return false;
    float planeLength=sqrt(planeSquared),along=dot(worldPlane,direction);
    // Explicit perpendicular avoids cancellation between squared lengths.
    float3 perpendicular=worldPlane-along*direction;
    float across=length(perpendicular);
    float support=along>=cosine*planeLength ? planeLength : cosine*along+sine*across;
    float maximum=range*max(support,0.0);
    float minimum=dot(plane,offset)-dot(abs(plane),cellExtent);
    float sourceMagnitude=max(max(abs(source.x),abs(source.y)),abs(source.z))+range;
    float linearMagnitude=dot(abs(plane),abs(column0)+abs(column1)+abs(column2));
    float errorScale=dot(abs(plane),abs(cellCenter)+cellExtent+abs(translation))+sourceMagnitude*linearMagnitude;
    if(!isfinite(along) || !isfinite(across) || !isfinite(support) || !isfinite(errorScale))return false;
    float margin=1e-4*max(1.0,errorScale);
    if(!isfinite(maximum) || !isfinite(minimum) || !isfinite(margin))return false;
    return minimum>maximum+margin;
}
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
    float3 cellCenter=(minimum+maximum)*.5,cellExtent=(maximum-minimum)*.5;
    uint base=cell*(clusterDimensions.w+1),count=0;
    for(uint lightIndex=0;lightIndex<clusterCounts.x;++lightIndex)
    {
        ClusterLight light=clusterLights[lightIndex];
        float3 center=mul(clusterView,float4(light.positionRange.xyz,1)).xyz;
        float radius=light.positionRange.w*clusterDepth.w;
        float3 delta=center-clamp(center,minimum,maximum);
        if(dot(delta,delta)<=radius*radius)
        {
            if(light.coneShadow.y==3.0 && ClusterSpotOutside(light,cellCenter,cellExtent,center))continue;
            // 용량 초과 표식은 전체 광원 순회로 전환한다.
            if(count==clusterDimensions.w){clusterList[base]=0xffffffffu;return;}
            clusterList[base+1+count++]=lightIndex;
        }
    }
    clusterList[base]=count;
}
