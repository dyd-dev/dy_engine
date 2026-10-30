#version 450
struct ClusterLight {vec4 positionRange;vec4 colorIntensity;vec4 directionOuterCos;vec4 coneShadow;};
layout(std430,set=0,binding=19) readonly buffer Lights {ClusterLight clusterLights[];};
layout(std430,set=0,binding=20) writeonly buffer Lists {uint clusterList[];};
layout(set=0,binding=21) uniform Parameters {mat4 view;mat4 projection;mat4 inverseProjection;vec4 depth;uvec4 dimensions;uvec4 counts;} clusters;
layout(local_size_x=64) in;
void main()
{
    uint cell=gl_GlobalInvocationID.x,total=clusters.dimensions.x*clusters.dimensions.y*clusters.dimensions.z;
    if(cell>=total)return;
    uvec3 index=uvec3(cell%clusters.dimensions.x,(cell/clusters.dimensions.x)%clusters.dimensions.y,
        cell/(clusters.dimensions.x*clusters.dimensions.y));
    vec2 lower=vec2(index.xy)/vec2(clusters.dimensions.xy)*2-1;
    vec2 upper=vec2(index.xy+uvec2(1))/vec2(clusters.dimensions.xy)*2-1;
    float begin=float(index.z)/float(clusters.dimensions.z),end=float(index.z+1)/float(clusters.dimensions.z);
    float nearDepth=clusters.counts.z!=0u?clusters.depth.x*exp(clusters.depth.z*begin):mix(clusters.depth.x,clusters.depth.y,begin);
    float farDepth=clusters.counts.z!=0u?clusters.depth.x*exp(clusters.depth.z*end):mix(clusters.depth.x,clusters.depth.y,end);
    vec3 minimum=vec3(1e30),maximum=vec3(-1e30);
    for(uint corner=0;corner<8;++corner)
    {
        vec2 xy=vec2((corner&1u)!=0u?upper.x:lower.x,(corner&2u)!=0u?upper.y:lower.y);
        vec4 projected=clusters.inverseProjection*vec4(xy,0,1);
        vec3 point=projected.xyz/projected.w;float distance=(corner&4u)!=0u?farDepth:nearDepth;
        if(clusters.counts.z!=0u)point*=distance/max(-point.z,1e-20);else point.z=-distance;
        minimum=min(minimum,point);maximum=max(maximum,point);
    }
    uint base=cell*(clusters.dimensions.w+1u),count=0u;
    for(uint lightIndex=0u;lightIndex<clusters.counts.x;++lightIndex)
    {
        ClusterLight light=clusterLights[lightIndex];
        vec3 center=(clusters.view*vec4(light.positionRange.xyz,1)).xyz;
        float radius=light.positionRange.w*clusters.depth.w;
        vec3 delta=center-clamp(center,minimum,maximum);
        if(dot(delta,delta)<=radius*radius)
        {
            // 셀 용량을 넘으면 픽셀 셰이더가 전체 광원을 순회하므로 광원을 누락하지 않는다.
            if(count==clusters.dimensions.w){clusterList[base]=0xffffffffu;return;}
            clusterList[base+1u+count++]=lightIndex;
        }
    }
    clusterList[base]=count;
}
