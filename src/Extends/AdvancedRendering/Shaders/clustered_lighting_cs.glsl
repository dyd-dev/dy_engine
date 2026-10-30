#version 450
struct ClusterLight {vec4 positionRange;vec4 colorIntensity;vec4 directionOuterCos;vec4 coneShadow;};
layout(std430,set=0,binding=19) readonly buffer Lights {ClusterLight clusterLights[];};
layout(std430,set=0,binding=20) writeonly buffer Lists {uint clusterList[];};
layout(set=0,binding=21) uniform Parameters {mat4 view;mat4 projection;mat4 inverseProjection;vec4 depth;uvec4 dimensions;uvec4 counts;} clusters;
layout(local_size_x=64) in;
shared ClusterLight lightBatch[64];
shared vec4 lightBounds[64];
bool ClusterFinite(vec3 value) {return !any(isnan(value)) && !any(isinf(value));}
bool ClusterSpotOutside(ClusterLight light,vec3 cellCenter,vec3 cellExtent,vec3 lightCenter)
{
    vec3 source=light.positionRange.xyz,direction=light.directionOuterCos.xyz;
    float range=light.positionRange.w,outerCos=light.directionOuterCos.w;
    mat3 viewLinear=mat3(clusters.view);
    if(!ClusterFinite(source) || !ClusterFinite(direction) || !ClusterFinite(cellCenter) ||
        !ClusterFinite(cellExtent) || !ClusterFinite(lightCenter) ||
        !ClusterFinite(viewLinear[0]) || !ClusterFinite(viewLinear[1]) || !ClusterFinite(viewLinear[2]) ||
        !ClusterFinite(clusters.view[3].xyz) || isnan(range) || isinf(range) ||
        isnan(outerCos) || isinf(outerCos) || range<=0 || outerCos<=0 || any(lessThan(cellExtent,vec3(0))))return false;
    float directionSquared=dot(direction,direction);
    if(isnan(directionSquared) || isinf(directionSquared) || directionSquared<=1e-12)return false;
    float directionLength=sqrt(directionSquared);
    direction/=directionLength;
    // Fragment lighting uses the original, slightly non-unit direction.
    // Widen its effective cone to retain floating-point boundary cases.
    float effectiveCos=outerCos/directionLength;
    if(isnan(effectiveCos) || isinf(effectiveCos))return false;
    float cosine=clamp(effectiveCos-1e-5,0.0,1.0);
    float sine=sqrt(max(1.0-cosine*cosine,0.0));
    vec3 axis=viewLinear*direction,offset=cellCenter-lightCenter;
    float axisSquared=dot(axis,axis);
    if(!ClusterFinite(offset) || isnan(axisSquared) || isinf(axisSquared) || axisSquared<=1e-12)return false;
    axis/=sqrt(axisSquared);
    vec3 radial=offset-axis*dot(offset,axis);
    float radialSquared=dot(radial,radial);
    if(isnan(radialSquared) || isinf(radialSquared) || radialSquared<=1e-12)return false;
    // This only chooses a useful separating plane; correctness does not
    // assume that the view preserves angles or lengths.
    vec3 plane=cosine*(radial/sqrt(radialSquared))-sine*axis;
    vec3 worldPlane=transpose(viewLinear)*plane;
    float planeSquared=dot(worldPlane,worldPlane);
    if(!ClusterFinite(plane) || !ClusterFinite(worldPlane) ||
        isnan(planeSquared) || isinf(planeSquared) || planeSquared<=1e-12)return false;
    float planeLength=sqrt(planeSquared),along=dot(worldPlane,direction);
    // Form the perpendicular explicitly instead of subtracting squared lengths.
    vec3 perpendicular=worldPlane-along*direction;
    float across=length(perpendicular);
    float support=along>=cosine*planeLength ? planeLength : cosine*along+sine*across;
    float maximum=range*max(support,0.0);
    float minimum=dot(plane,offset)-dot(abs(plane),cellExtent);
    float sourceMagnitude=max(max(abs(source.x),abs(source.y)),abs(source.z))+range;
    float linearMagnitude=dot(abs(plane),abs(viewLinear[0])+abs(viewLinear[1])+abs(viewLinear[2]));
    float errorScale=dot(abs(plane),abs(cellCenter)+cellExtent+abs(clusters.view[3].xyz))+
        sourceMagnitude*linearMagnitude;
    if(isnan(along) || isinf(along) || isnan(across) || isinf(across) ||
        isnan(support) || isinf(support) || isnan(errorScale) || isinf(errorScale))return false;
    float margin=1e-4*max(1.0,errorScale);
    if(isnan(maximum) || isinf(maximum) || isnan(minimum) || isinf(minimum) ||
        isnan(margin) || isinf(margin))return false;
    return minimum>maximum+margin;
}
void main()
{
    uint cell=gl_GlobalInvocationID.x,total=clusters.dimensions.x*clusters.dimensions.y*clusters.dimensions.z;
    uint lane=gl_LocalInvocationIndex;
    bool validCell=cell<total,overflow=false;
    vec3 minimum=vec3(1e30),maximum=vec3(-1e30);
    if(validCell)
    {
        uvec3 index=uvec3(cell%clusters.dimensions.x,(cell/clusters.dimensions.x)%clusters.dimensions.y,
            cell/(clusters.dimensions.x*clusters.dimensions.y));
        vec2 lower=vec2(index.xy)/vec2(clusters.dimensions.xy)*2-1;
        vec2 upper=vec2(index.xy+uvec2(1))/vec2(clusters.dimensions.xy)*2-1;
        float begin=float(index.z)/float(clusters.dimensions.z),end=float(index.z+1)/float(clusters.dimensions.z);
        float nearDepth=clusters.counts.z!=0u?clusters.depth.x*exp(clusters.depth.z*begin):mix(clusters.depth.x,clusters.depth.y,begin);
        float farDepth=clusters.counts.z!=0u?clusters.depth.x*exp(clusters.depth.z*end):mix(clusters.depth.x,clusters.depth.y,end);
        for(uint corner=0;corner<8;++corner)
        {
            vec2 xy=vec2((corner&1u)!=0u?upper.x:lower.x,(corner&2u)!=0u?upper.y:lower.y);
            vec4 projected=clusters.inverseProjection*vec4(xy,0,1);
            vec3 point=projected.xyz/projected.w;float distance=(corner&4u)!=0u?farDepth:nearDepth;
            if(clusters.counts.z!=0u)point*=distance/max(-point.z,1e-20);else point.z=-distance;
            minimum=min(minimum,point);maximum=max(maximum,point);
        }
    }
    vec3 cellCenter=(minimum+maximum)*.5,cellExtent=(maximum-minimum)*.5;
    uint base=validCell?cell*(clusters.dimensions.w+1u):0u,count=0u,first=0u;
    while(first<clusters.counts.x)
    {
        uint batchCount=min(64u,clusters.counts.x-first);
        if(lane<batchCount)
        {
            ClusterLight light=clusterLights[first+lane];
            lightBatch[lane]=light;
            lightBounds[lane]=vec4((clusters.view*vec4(light.positionRange.xyz,1)).xyz,
                light.positionRange.w*clusters.depth.w);
        }
        // Inactive/overflow cells still load their lane and reach both barriers.
        barrier();
        if(validCell && !overflow)
            for(uint local=0u;local<batchCount && !overflow;++local)
            {
                ClusterLight light=lightBatch[local];
                vec3 center=lightBounds[local].xyz;
                float radius=lightBounds[local].w;
                vec3 delta=center-clamp(center,minimum,maximum);
                if(dot(delta,delta)<=radius*radius)
                {
                    if(light.coneShadow.y==3.0 && ClusterSpotOutside(light,cellCenter,cellExtent,center))continue;
                    // Overflow retains the complete-light-list fallback.
                    if(count==clusters.dimensions.w)overflow=true;
                    else clusterList[base+1u+count++]=first+local;
                }
            }
        barrier();
        first+=batchCount;
    }
    if(validCell)clusterList[base]=overflow?0xffffffffu:count;
}
