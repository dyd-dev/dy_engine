#include <metal_stdlib>
using namespace metal;
#ifndef GPU_VISIBILITY_OCCLUSION // 가림 빌드 여부를 지정하지 않으면 절두체·LOD만 수행하는 경로를 기본으로 한다.
#define GPU_VISIBILITY_OCCLUSION 0
#endif
// 입력 하나에 현재 model, 이전 프레임 previousMVP, 경계 구, 최대 4단계 LOD 오차·배치 번호를 담는다. metadata.x는 LOD 수, metadata.y의 비트 0은 경계 구 유효 표시다.
// CPU와 맞추는 버퍼 항목 간격은 Instance 192바이트, Batch 32바이트, VisibleInstance 128바이트다. 입력과 압축 출력의 간격을 혼동하면 다음 물체의 행렬부터 어긋난다.
struct Instance
{
    float4x4 model,previousMVP;
    float4 centerRadius,lodErrors;
    uint4 batches,metadata;
};
// 배치 하나의 인덱스 범위, 정점 시작 보정, 출력 인스턴스 구간과 용량이다. CPU가 배치별 최악의 수용량을 검증한 뒤 보낸다.
struct Batch {uint indexCount,firstIndex;int vertexOffset;uint instanceOffset;uint4 capacityPadding;};
// 살아남은 인스턴스도 model과 previousMVP를 함께 옮긴다. 현재·이전 변환의 짝을 유지해 후속 표면/모션 단계가 같은 물체의 이력을 읽게 한다.
struct VisibleInstance {float4x4 model,previousMVP;};
// planes는 정규화된 절두체 6면, depthRow는 시점 앞쪽 거리를 구하는 행이다.
// 상태 전환 방향마다 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)의 폭을 projection에 담는다.
// projection은 픽셀 배율·허용 오차·히스테리시스 폭·원근 여부, counts는 물체 수·배치 수·절두체 검사·LOD 사용 순서다.
struct Frame {float4 planes[6];float4 depthRow,projection,lodScreenSizes;uint4 counts;};
#if GPU_VISIBILITY_OCCLUSION // 가림 빌드에서만 Hi-Z 자원·판정 함수·제거 조건을 포함한다.
struct OcclusionFrame {float4x4 viewProjection;float4 viewport;uint4 info;};
// 경계 구를 감싸는 상자의 8개 꼭짓점을 투영해 화면 사각형과 가장 가까운 깊이를 구한다. near plane을 걸치거나 수치가 불안정하면 제거하지 않는다.
// 화면 사각형을 여유 픽셀만큼 넓힌 뒤 포함하는 Hi-Z 셀을 모두 읽는다. 가장 먼 차폐 깊이에도 여유값을 더한 결과가 물체보다 앞일 때만 가렸다고 판단한다.
bool Occluded(float4 sphere,constant OcclusionFrame& occlusion,texture2d<float> hizImage)
{
    float2 low=1e30f,high=-1e30f;float nearestDepth=1;
    for(uint corner=0;corner<8;++corner)
    {
        float3 signValue=float3((corner&1u)!=0?1:-1,(corner&2u)!=0?1:-1,(corner&4u)!=0?1:-1);
        float4 clip=occlusion.viewProjection*float4(sphere.xyz+signValue*sphere.w,1);
        if(!all(isfinite(clip)) || clip.w<=1e-6f || clip.z<=0) return false; // 투영이 비유한값이거나 카메라 뒤/near plane에 걸치면 확실히 가렸다고 판단하지 않는다.
        float3 ndc=clip.xyz/clip.w;
        float2 pixel=(ndc.xy*float2(.5,-.5)+.5)*occlusion.viewport.xy;
        low=min(low,pixel);high=max(high,pixel);nearestDepth=min(nearestDepth,ndc.z);
    }
    if(high.x<0 || high.y<0 || low.x>=occlusion.viewport.x || low.y>=occlusion.viewport.y) return false; // 화면 밖 사각형은 이 가림 단계에서 제거하지 않는다. 시야 제거는 별도 절두체 검사가 맡는다.
    low=clamp(floor(low-occlusion.viewport.w),0.f,occlusion.viewport.xy-1);
    high=clamp(ceil(high+occlusion.viewport.w)-1,0.f,occlusion.viewport.xy-1);
    float2 extent=high-low+1;
    int level=clamp(int(ceil(log2(max(extent.x,extent.y))))-2,0,int(occlusion.info.x)-1);
    // Hi-Z 0단계는 원본 깊이의 4×4를 대표한다. 이후 단계마다 2배 넓어져 한 셀의 원본 폭은 4<<level이다.
    float footprint=float(4u<<uint(level));
    int2 size=int2(hizImage.get_width(uint(level)),hizImage.get_height(uint(level)));
    int2 first=clamp(int2(floor(low/footprint)),int2(0),size-1);
    int2 last=clamp(int2(floor(high/footprint)),int2(0),size-1);
    float blocker=0;
    for(int y=first.y;y<=last.y;++y) for(int x=first.x;x<=last.x;++x)
    {
        float depth=hizImage.read(uint2(x,y),uint(level)).r;
        if(!isfinite(depth) || depth>=1) return false; // 셀 깊이가 불명확하거나 미피복 깊이 1이면 사각형 전체의 가림을 확정할 수 없다.
        blocker=max(blocker,depth);
    }
    return isfinite(nearestDepth) && blocker+occlusion.viewport.z<nearestDepth;
}
#endif
// 명령 x=0이면 간접 인수·선택적 LOD 이력을 초기화한다. 그 외에는 물체 하나씩 검사하며, 출력 위치를 원자적으로 확보해 배치별 구간에 기록한다.
kernel void kernelMain(uint3 threadId [[thread_position_in_grid]],
    device const Instance* inputs [[buffer(0)]],device const Batch* batches [[buffer(1)]],
    device VisibleInstance* visible [[buffer(2)]],device atomic_uint* arguments [[buffer(3)]],
    device uint* history [[buffer(4)]],constant Frame& frame [[buffer(5)]],
#if GPU_VISIBILITY_OCCLUSION // 가림 빌드에서만 Hi-Z 자원·판정 함수·제거 조건을 포함한다.
    texture2d<float> hizImage [[texture(6)]],constant OcclusionFrame& occlusion [[buffer(7)]],sampler hizSampler [[sampler(8)]],
#endif
    constant uint4& operation [[buffer(15)]])
{
    // 2차원 dispatch를 operation.y의 실제 스레드 행 폭으로 펼친다. dispatch 그룹 수 자체를 곱하는 것이 아니다.
    uint id=threadId.x+threadId.y*operation.y;
    if(operation.x==0) // 초기화 명령이면 배치 간접 인수와 요청된 LOD 이력만 쓰고 종료한다.
    {
        if(id<frame.counts.y) // 실제 배치 수 안의 스레드만 해당 배치의 인수 5개를 초기화한다.
        {
            // 간접 인수는 uint 5개씩: indexCount, instanceCount, firstIndex, vertexOffset, firstInstance다. 선택 패스가 두 번째 칸의 개수만 증가시킨다.
            Batch batch=batches[id];uint offset=id*5;
            atomic_store_explicit(&arguments[offset],batch.indexCount,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+1],0,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+2],batch.firstIndex,memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+3],as_type<uint>(batch.vertexOffset),memory_order_relaxed);
            atomic_store_explicit(&arguments[offset+4],operation.w!=0?batch.instanceOffset:0,memory_order_relaxed);
        }
        if(operation.z!=0 && id<frame.counts.x) history[id]=0; // 이력 재설정 요청이 있고 물체 인덱스가 유효할 때만 LOD를 0으로 되돌린다.
        return;
    }
    if(id>=frame.counts.x) return; // dispatch를 올림해 생긴 실제 물체 수 밖의 스레드는 아무 자료도 읽지 않는다.
    Instance input=inputs[id];
    // 경계가 없거나 반지름·중심이 유효하지 않으면 절두체·가림 제거와 LOD 축소를 건너뛰어 기본 LOD를 남긴다.
    bool bounded=(input.metadata.y&1u)!=0 && input.centerRadius.w>=0 && all(isfinite(input.centerRadius));
    if(frame.counts.z!=0 && bounded) // 절두체 검사가 켜져 있고 경계 구가 유효한 물체만 6개 평면으로 검사한다.
        for(uint plane=0;plane<6;++plane)
        {
            float4 p=frame.planes[plane];
            float distance=dot(p,float4(input.centerRadius.xyz,1));
            float tolerance=1e-5f*(dot(abs(p.xyz),abs(input.centerRadius.xyz))+abs(p.w)+input.centerRadius.w+1);
            if(distance < -input.centerRadius.w-tolerance) return; // 오차 여유까지 포함한 구 전체가 평면 밖에 있을 때만 제거한다.
        }
    // 기본은 가장 상세한 0단계다. 거리로 월드 오차를 픽셀 오차로 환산하고, 임곗값을 넘기 전까지 단계 번호를 올린다.
    uint selected=0;
    if(frame.counts.w!=0 && bounded) // LOD가 켜져 있고 경계 구가 유효할 때만 화면 오차로 단계를 선택한다.
    {
        float depth=dot(frame.depthRow,float4(input.centerRadius.xyz,1));
        bool perspective=frame.projection.w>.5f;
        float pixels=frame.projection.x*(perspective?1.f/max(depth-input.centerRadius.w,.001f):1.f);
        float screenSize=2.0f*input.centerRadius.w*pixels;
        // 픽셀 배율이 유한한 0 이상일 때만 LOD를 낮추며, 원근 투영에서는 경계 구 전체가 카메라 앞에 있어야 한다.
        if((!perspective || depth>input.centerRadius.w+.001f) && isfinite(pixels) && pixels>=0 && isfinite(screenSize))
        {
            uint previous=min(history[id],input.metadata.x-1);
            for(uint level=1;level<input.metadata.x;++level)
            {
                float error=input.lodErrors[level]*pixels;
                // 이전 LOD보다 거칠게 바꿀 때는 더 엄격한 한도, 되돌아올 때는 느슨한 한도를 써 경계에서 LOD가 왕복하는 현상을 줄인다.
                float transition=level>previous?1-frame.projection.z:1+frame.projection.z;
                float limit=frame.projection.y*transition;
                if(!isfinite(error) || error<0 || error>limit || screenSize>frame.lodScreenSizes[level-1]*transition) break; // 화면 크기와 오차 기준을 모두 만족하는 축약 단계만 허용한다.
                selected=level;
            }
        }
    }
    // LOD 이력은 가림 판정 전에 갱신한다. 이 뒤 Hi-Z에 가려져 그리지 않더라도 이번 선택 결과가 다음 프레임에 남는다.
    history[id]=selected;
#if GPU_VISIBILITY_OCCLUSION // 가림 빌드에서만 Hi-Z 자원·판정 함수·제거 조건을 포함한다.
    if(bounded && Occluded(input.centerRadius,occlusion,hizImage)) return; // 유효한 경계 구의 전체 화면 영역이 가렸다고 확정된 물체만 출력에서 제외한다.
#endif
    // 선택 LOD의 배치 번호로 출력 구간을 정하고 원자 증가가 돌려준 이전 개수를 슬롯으로 사용한다. 순서는 스레드 실행 순서에 따라 달라진다.
    uint batchId=input.batches[selected];Batch batch=batches[batchId];
    uint index=atomic_fetch_add_explicit(&arguments[batchId*5+1],1,memory_order_relaxed);
    if(index>=batch.capacityPadding.x) return; // 출력 버퍼 범위를 넘는 쓰기를 막는다. 원자 증가된 인수 개수는 되돌리지 않으므로 CPU의 용량 검증이 전제다.
    visible[batch.instanceOffset+index]={input.model,input.previousMVP};
}
