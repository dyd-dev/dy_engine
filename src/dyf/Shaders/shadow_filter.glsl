// 카메라·프레임 난수 없이 같은 광원 좌표에서 같은 표본을 사용한다.
vec2 DyShadowDiskOffset(int sampleIndex,int count)
{
    const vec2 directions[16]=vec2[16](
        vec2(1.000000000,0.000000000),
        vec2(-0.737368878,0.675490294),
        vec2(0.087425725,-0.996171041),
        vec2(0.608438861,0.793600751),
        vec2(-0.984713485,-0.174181950),
        vec2(0.843755295,-0.536728053),
        vec2(-0.259604305,0.965715074),
        vec2(-0.460907025,-0.887448429),
        vec2(0.939321296,0.343038631),
        vec2(-0.924345556,0.381556408),
        vec2(0.423845995,-0.905734273),
        vec2(0.299283864,0.954164120),
        vec2(-0.865211210,-0.501407581),
        vec2(0.976675774,-0.214719429),
        vec2(-0.575129429,0.818062430),
        vec2(-0.128510690,-0.991708124));
    int pairs=count/2;
    if(sampleIndex>=pairs*2)return vec2(0);
    int pairIndex=sampleIndex/2;
    return directions[pairIndex]*sqrt((float(pairIndex)+.5)/float(max(pairs,1)))*(sampleIndex%2==0?1.0:-1.0);
}

int DyShadowCubeFace(vec3 direction)
{
    vec3 magnitude=abs(direction);
    if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)return direction.x>=0?0:1;
    if(magnitude.y>=magnitude.z)return direction.y>=0?2:3;
    return direction.z>=0?4:5;
}

float DyShadowLinearDepth(int index,float depth)
{
    vec4 projection=DY_SHADOW_PROJECTION(index);
    if(projection.w==0)return depth*projection.z;
    float denominator=depth+projection.z;
    return denominator<0?projection.w/denominator:1e20;
}

float DyShadowDepthTap(int index,int pointBase,vec2 coordinate)
{
    float radialScale=1;
    if(pointBase>=0)
    {
        // 큐브면 밖의 표본은 방향을 복원해 이웃 면으로 보내고 방사 거리로 비교한다.
        mat4 matrix=DY_SHADOW_MATRIX(index);
        vec4 projection=DY_SHADOW_PROJECTION(index);
        vec3 ray=vec3(matrix[0][3],matrix[1][3],matrix[2][3])+
            vec3(matrix[0][0],matrix[1][0],matrix[2][0])*((coordinate.x*2-1)/(4*projection.x*projection.x))+
            vec3(matrix[0][1],matrix[1][1],matrix[2][1])*((1-coordinate.y*2)/(4*projection.y*projection.y));
        index=pointBase+DyShadowCubeFace(ray);
        vec4 clip=DY_SHADOW_MATRIX(index)*vec4(ray,0);
        if(clip.w<=0)return 1e20;
        coordinate=clip.xy/clip.w*vec2(.5,-.5)+.5;
        radialScale=length(ray)/clip.w;
    }
    ivec2 dimensions=DY_SHADOW_DIMENSIONS;
    vec4 tile=DY_SHADOW_TILE(index);
    ivec2 minimum=ivec2(round(tile.xy*vec2(dimensions)));
    ivec2 maximum=minimum+ivec2(round(tile.zw*vec2(dimensions)))-1;
    ivec2 pixel=clamp(ivec2(floor((tile.xy+coordinate*tile.zw)*vec2(dimensions))),minimum,maximum);
    float depth=DY_SHADOW_READ(pixel);
    // 비어 있는 깊이 1은 실제 차폐물 거리로 누적하지 않는다.
    return depth>=1?1e20:DyShadowLinearDepth(index,depth)*radialScale;
}

float DyShadowPcss(int index,int pointBase,vec2 coordinate,float receiverNdc,float clipDistance,
    vec3 toLight,float sourceRadius)
{
    vec4 projection=DY_SHADOW_PROJECTION(index);
    vec4 tile=DY_SHADOW_TILE(index);
    vec2 texel=1.0/(vec2(DY_SHADOW_DIMENSIONS)*tile.zw);
    vec2 maximumRadius=texel*DY_SHADOW_SOFT.x;
    bool perspective=projection.w<0;
    if(!(projection.x>0 && projection.y>0))return 1;
    float receiver=DyShadowLinearDepth(index,receiverNdc);
    if(pointBase>=0)
    {
        mat4 matrix=DY_SHADOW_MATRIX(index);
        receiver/=max(abs(dot(toLight,vec3(matrix[0][3],matrix[1][3],matrix[2][3]))),1e-6);
    }
    if(!(receiver>0))return 1;
    float nearPlane=perspective?projection.w/projection.z:0;
    vec2 searchRadius=perspective?
        sourceRadius*projection.xy*max(clipDistance-nearPlane,0.0)/max(nearPlane*clipDistance,1e-12):
        sourceRadius*projection.xy*receiver;
    searchRadius=min(searchRadius,maximumRadius);
    int blockerCount=clamp(int(DY_SHADOW_FILTER.x+.5),1,32);
    float blockerSum=0,blockers=0;
    for(int sampleIndex=0;sampleIndex<blockerCount;++sampleIndex)
    {
        float depth=DyShadowDepthTap(index,pointBase,coordinate+DyShadowDiskOffset(sampleIndex,blockerCount)*searchRadius);
        if(depth<receiver){blockerSum+=depth;blockers+=1;}
    }
    if(blockers==0)return 1;
    float blocker=blockerSum/blockers;
    float penumbra=sourceRadius*max(receiver-blocker,0.0);
    if(perspective)penumbra/=max(blocker,nearPlane)*max(clipDistance,nearPlane);
    vec2 filterRadius=max(min(penumbra*projection.xy,maximumRadius),min(texel*.5,maximumRadius));
    int filterCount=clamp(int(DY_SHADOW_FILTER.y+.5),1,32);
    float visible=0;
    for(int sampleIndex=0;sampleIndex<filterCount;++sampleIndex)
    {
        float depth=DyShadowDepthTap(index,pointBase,coordinate+DyShadowDiskOffset(sampleIndex,filterCount)*filterRadius);
        visible+=receiver<=depth?1:0;
    }
    return visible/float(filterCount);
}
#undef DY_SHADOW_PROJECTION
#undef DY_SHADOW_MATRIX
#undef DY_SHADOW_TILE
#undef DY_SHADOW_DIMENSIONS
#undef DY_SHADOW_READ
#undef DY_SHADOW_FILTER
#undef DY_SHADOW_SOFT
