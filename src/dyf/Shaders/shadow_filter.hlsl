float2 DyShadowDiskOffset(int sampleIndex,int count)
{
    const float2 directions[16]={
        float2(1.000000000,0.000000000),
        float2(-0.737368878,0.675490294),
        float2(0.087425725,-0.996171041),
        float2(0.608438861,0.793600751),
        float2(-0.984713485,-0.174181950),
        float2(0.843755295,-0.536728053),
        float2(-0.259604305,0.965715074),
        float2(-0.460907025,-0.887448429),
        float2(0.939321296,0.343038631),
        float2(-0.924345556,0.381556408),
        float2(0.423845995,-0.905734273),
        float2(0.299283864,0.954164120),
        float2(-0.865211210,-0.501407581),
        float2(0.976675774,-0.214719429),
        float2(-0.575129429,0.818062430),
        float2(-0.128510690,-0.991708124)};
    int pairs=count/2;
    if(sampleIndex>=pairs*2)return float2(0,0); // 표본 수가 홀수여서 쌍에 속하지 않는 마지막 표본은 중심에 둔다.
    int pairIndex=sampleIndex/2;
    return directions[pairIndex]*sqrt((float(pairIndex)+.5)/float(max(pairs,1)))*(sampleIndex%2==0?1.0:-1.0);
}

// 방향의 절댓값이 가장 큰 축과 부호로 +X/-X/+Y/-Y/+Z/-Z 시점 번호를 고른다.
int DyShadowCubeFace(float3 direction)
{
    float3 magnitude=abs(direction);
    if(magnitude.x>=magnitude.y && magnitude.x>=magnitude.z)return direction.x>=0?0:1; // X축 절댓값이 가장 크면 부호로 +X 또는 -X 면을 고른다.
    if(magnitude.y>=magnitude.z)return direction.y>=0?2:3; // X축을 고르지 않았고 Y축이 Z축 이상이면 +Y 또는 -Y 면을 고른다.
    return direction.z>=0?4:5;
}

// CPU가 준 투영 계수로 저장 깊이를 광원 시점 거리로 복원한다. 직교 투영은 선형 배율, 원근 투영은 분모 역수를 사용한다.
// 원근 복원의 depth + projection.z가 음수가 아니면 깊이 복원을 하지 않고 아주 먼 거리값인 1e20을 반환한다.
float DyShadowLinearDepth(int index,float depth)
{
    float4 projection=DY_SHADOW_PROJECTION(index);
    if(projection.w==0)return depth*projection.z; // 직교 투영은 저장 깊이에 거리 배율만 곱한다.
    float denominator=depth+projection.z;
    return denominator<0?projection.w/denominator:1e20;
}

// 한 표본을 읽는다. 점광원은 현재 면의 UV에서 광선 방향을 복원해 실제 큐브면을 다시 고르고, 해당 면 깊이를 방사 거리로 바꾼다.
// 아틀라스 타일의 정수 픽셀 경계를 구해 표본 좌표를 제한하므로 다른 광원의 타일을 읽지 않는다.
float DyShadowDepthTap(int index,int pointBase,float2 coordinate)
{
    float radialScale=1;
    if(pointBase>=0) // 점광원 시점이면 큐브면 깊이를 광원 중심으로부터의 방사 거리와 맞춘다.
    {
        // 큐브면 밖의 표본은 방향을 복원해 이웃 면으로 보내고 방사 거리로 비교한다.
        float4x4 matrix=DY_SHADOW_MATRIX(index);
        float4 projection=DY_SHADOW_PROJECTION(index);
        // HLSL은 matrix[행]을 바로 읽는다. GLSL·Metal은 열 우선 인덱스로 같은 행 성분을 모아 면의 광선 방향을 만든다.
        float3 ray=matrix[3].xyz+
            matrix[0].xyz*((coordinate.x*2-1)/(4*projection.x*projection.x))+
            matrix[1].xyz*((1-coordinate.y*2)/(4*projection.y*projection.y));
        index=pointBase+DyShadowCubeFace(ray);
        float4 clip=mul(DY_SHADOW_MATRIX(index),float4(ray,0));
        if(clip.w<=0)return 1e20; // 새 큐브면 앞으로 투영할 수 없는 표본은 차폐물로 세지 않는다.
        coordinate=clip.xy/clip.w*float2(.5,-.5)+.5;
        radialScale=length(ray)/clip.w;
    }
    int2 dimensions=DY_SHADOW_DIMENSIONS;
    float4 tile=DY_SHADOW_TILE(index);
    int2 minimum=int2(round(tile.xy*float2(dimensions)));
    int2 maximum=minimum+int2(round(tile.zw*float2(dimensions)))-1;
    int2 pixel=clamp(int2(floor((tile.xy+coordinate*tile.zw)*float2(dimensions))),minimum,maximum);
    float depth=DY_SHADOW_READ(pixel);
    // 비어 있는 깊이 1은 실제 차폐물 거리로 누적하지 않는다.
    return depth>=1?1e20:DyShadowLinearDepth(index,depth)*radialScale;
}

// 수신 깊이 복원 → 광원 크기로 차폐물 탐색 반경 계산 → 앞에 있는 표본의 평균 거리 → 반그림자 필터 반경 → 가시 표본 비율 순서다.
// 필터 반경은 softShadowParams.x의 타일 텍셀 한도를 넘지 않고, 표본 수는 각각 1~32로 제한한다.
float DyShadowPcss(int index,int pointBase,float2 coordinate,float receiverNdc,float clipDistance,
    float3 toLight,float sourceRadius)
{
    float4 projection=DY_SHADOW_PROJECTION(index);
    float4 tile=DY_SHADOW_TILE(index);
    float2 texel=1.0/(float2(DY_SHADOW_DIMENSIONS)*tile.zw);
    float2 maximumRadius=texel*DY_SHADOW_SOFT.x;
    bool perspective=projection.w<0;
    if(!(projection.x>0 && projection.y>0))return 1; // UV 배율이 유효한 양수가 아니면 그림자로 단정하지 않고 완전 가시로 둔다.
    float receiver=DyShadowLinearDepth(index,receiverNdc);
    if(pointBase>=0) // 점광원 시점이면 큐브면 깊이를 광원 중심으로부터의 방사 거리와 맞춘다.
    {
        float4x4 matrix=DY_SHADOW_MATRIX(index);
        receiver/=max(abs(dot(toLight,matrix[3].xyz)),1e-6);
    }
    if(!(receiver>0))return 1; // 복원된 수신 거리가 양수가 아니면 가시율 1로 종료한다.
    float nearPlane=perspective?projection.w/projection.z:0;
    // 원근 투영은 광원 near plane과 clip 거리를 반영하고, 직교 투영은 수신 거리와 광원 크기를 사용한다. 둘 다 현재 타일 UV 단위의 반경이다.
    float2 searchRadius=perspective?
        sourceRadius*projection.xy*max(clipDistance-nearPlane,0.0)/max(nearPlane*clipDistance,1e-12):
        sourceRadius*projection.xy*receiver;
    searchRadius=min(searchRadius,maximumRadius);
    int blockerCount=clamp(int(DY_SHADOW_FILTER.x+.5),1,32);
    float blockerSum=0,blockers=0;
    // Paired taps share the direction and radius; keep their accumulation order.
    for(int sampleIndex=0;sampleIndex<blockerCount;sampleIndex+=2)
    {
        float2 offset=DyShadowDiskOffset(sampleIndex,blockerCount);
        float depth=DyShadowDepthTap(index,pointBase,coordinate+offset*searchRadius);
        if(depth<receiver){blockerSum+=depth;blockers+=1;} // 수신점보다 가까운 깊이만 차폐물 평균에 포함한다.
        if(sampleIndex+1<blockerCount)
        {
            depth=DyShadowDepthTap(index,pointBase,coordinate+(-offset)*searchRadius);
            if(depth<receiver){blockerSum+=depth;blockers+=1;} // 수신점보다 가까운 깊이만 차폐물 평균에 포함한다.
        }
    }
    if(blockers==0)return 1; // 탐색 표본에 앞쪽 차폐물이 없으면 완전 가시로 반환한다.
    // 차폐물 평균 거리보다 수신점이 멀수록 반그림자가 넓어진다. 원근 투영에서는 차폐물·수신점 거리에 맞춰 다시 나눈다.
    float blocker=blockerSum/blockers;
    float penumbra=sourceRadius*max(receiver-blocker,0.0);
    if(perspective)penumbra/=max(blocker,nearPlane)*max(clipDistance,nearPlane);
    float2 filterRadius=max(min(penumbra*projection.xy,maximumRadius),min(texel*.5,maximumRadius));
    int filterCount=clamp(int(DY_SHADOW_FILTER.y+.5),1,32);
    float visible=0;
    for(int sampleIndex=0;sampleIndex<filterCount;sampleIndex+=2)
    {
        float2 offset=DyShadowDiskOffset(sampleIndex,filterCount);
        float depth=DyShadowDepthTap(index,pointBase,coordinate+offset*filterRadius);
        visible+=receiver<=depth?1:0;
        if(sampleIndex+1<filterCount)
        {
            depth=DyShadowDepthTap(index,pointBase,coordinate+(-offset)*filterRadius);
            visible+=receiver<=depth?1:0;
        }
    }
    return visible/float(filterCount);
}
// 포함 파일이 사용한 연결 매크로를 해제해 다음 셰이더 코드에 이름이 남지 않게 한다.
#undef DY_SHADOW_PROJECTION
#undef DY_SHADOW_MATRIX
#undef DY_SHADOW_TILE
#undef DY_SHADOW_DIMENSIONS
#undef DY_SHADOW_READ
#undef DY_SHADOW_FILTER
#undef DY_SHADOW_SOFT
