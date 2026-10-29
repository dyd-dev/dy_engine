#include "GpuVisibility.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include "dyf/RHI/Pipeline.h"
#include "dyf/RHI/Binding.h"
#include "dyf/RHI/Shader.h"
#include "dyf/RHI/Indirect.h"
#include "dyf/RHI/Texture.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // 컴파일 시 해당 GPU 백엔드용으로 생성된 가시성·가림·HiZ 셰이더 바이트를 포함한다.
#include "GpuVisibilityCompute.h"
#include "GpuOcclusionCompute.h"
#include "GpuHiZCompute.h"
#endif

namespace dyf
{
namespace
{
struct OcclusionLayouts
{
    std::array<RHI::ResourceBindingLayout,5> hiz;
    std::array<RHI::ResourceBindingLayout,9> occlusion;
    OcclusionLayouts()
    {
        using namespace RHI;
        // HiZ/가림 깊이는 셀 값을 그대로 읽고 주소 범위는 가장자리로 제한한다.
        SamplerDesc sampler;
        sampler.minFilter=sampler.magFilter=sampler.mipFilter=SamplerFilter::Nearest;
        sampler.addressU=sampler.addressV=sampler.addressW=SamplerAddressMode::ClampToEdge;
        sampler.mipLodBias=sampler.minLod=0;sampler.maxLod=32;
        hiz={{{0,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
            {2,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
            {3,ResourceBindingType::StorageTexture,1,ShaderStageFlags::Compute,{}},
            {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler}}};
        occlusion={{{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {3,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
            {5,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {6,ResourceBindingType::SampledTexture,1,ShaderStageFlags::Compute,{}},
            {7,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}},
            {8,ResourceBindingType::StaticSampler,1,ShaderStageFlags::Compute,sampler}}};
    }
    RHI::PipelineLayoutDesc HiZ() const
    {return {hiz.data(),static_cast<uint32_t>(hiz.size()),16,RHI::ShaderStageFlags::Compute,15};}
    RHI::PipelineLayoutDesc Occlusion() const
    {return {occlusion.data(),static_cast<uint32_t>(occlusion.size()),16,RHI::ShaderStageFlags::Compute,15};}
};

bool DefinitelyOutside(const GpuVisibility::Instance& instance,const std::array<Math::float4,6>& planes)
{
    const auto& sphere=instance.centerRadius;
    if(!(instance.flags&1u) || sphere.w<0 || !std::isfinite(sphere.x) || !std::isfinite(sphere.y) ||
        !std::isfinite(sphere.z) || !std::isfinite(sphere.w))return false;
    for(const auto& plane:planes)
    {
        const double x=double(plane.x)*sphere.x,y=double(plane.y)*sphere.y,z=double(plane.z)*sphere.z;
        const double magnitude=std::abs(x)+std::abs(y)+std::abs(z)+std::abs(double(plane.w))+sphere.w+1;
        // The shader uses 1e-5 of this magnitude. Double precision and twice
        // that margin cover f32 dot/add rounding, including fused operations.
        // Avoid predicting any case whose shader intermediates could overflow.
        if(!std::isfinite(magnitude) || magnitude>double(std::numeric_limits<float>::max())/16)continue;
        const double distance=x+y+z+plane.w;
        if(distance < -double(sphere.w)-2e-5*magnitude)return true;
    }
    return false;
}
}

// 버퍼와 이를 참조하는 바인딩을 먼저 정리하고, 공유 파이프라인·셰이더를 나중에 파괴한다.
GpuVisibility::~GpuVisibility()
{
    ReleaseBuffers();
    m_device.DestroyPipeline(m_hizPipeline);m_device.DestroyPipeline(m_occlusionPipeline);
    m_device.DestroyShader(m_hizShader);m_device.DestroyShader(m_occlusionShader);
    m_device.DestroyPipeline(m_pipeline);m_device.DestroyShader(m_shader);
}

// 입력 크기나 출력 용량이 달라질 때 사용한다. 버퍼를 참조하는 가림 자원도 함께 폐기하고 이력 초기화를 요구한다.
void GpuVisibility::ReleaseBuffers()
{
    ReleaseOcclusion();
    m_device.DestroyResourceSet(m_resources);m_resources=nullptr;
    for(auto* buffer:{m_inputs,m_batchData,m_frame,m_instances,m_arguments,m_lodHistory}) m_device.DestroyBuffer(buffer);
    m_inputs=m_batchData=m_frame=m_instances=m_arguments=m_lodHistory=nullptr;
    m_uploadReady=false;m_ready=false;m_pending=false;m_resetHistory=true;m_capacity=0;
    m_uploadDirty.fill(true);
    m_possibleBatches.clear();
    // 이전 그래프에 연결했던 핸들을 무효화해 새 버퍼가 옛 그래프 자원처럼 쓰이지 않게 한다.
    m_instancesGraph={};m_argumentsGraph={};
}

// 64비트로 계산한 크기를 장치/RHI 한도와 대조한 후에만 uint32_t로 좁혀 생성한다.
bool GpuVisibility::CreateBuffer(RHI::BufferHandle& buffer,uint64_t bytes,uint32_t stride,
    RHI::BufferUsage usage,RHI::ResourceState initialState)
{
    if(!bytes || bytes>UINT32_MAX || // 0 크기·32비트 초과·저장 버퍼 한도 초과를 모두 거부한다.
        ((usage&RHI::BufferUsage::Storage)!=RHI::BufferUsage::None && bytes>m_device.GetLimit(RHI::Limit::StorageBufferBytes))) return false;
    buffer=m_device.CreateBuffer({static_cast<uint32_t>(bytes),stride,usage,initialState});
    return buffer!=nullptr;
}

// compute와 간접 드로를 모두 지원하는 장치에서만 기본 선택 파이프라인을 준비한다. 생성한 파이프라인은 크기 변경 때도 재사용한다.
bool GpuVisibility::PreparePipeline()
{
    using namespace RHI;
    if(m_pipeline) return true; // 이미 생성된 기본 compute 파이프라인이면 그대로 사용한다.
    if(!m_device.Supports(Feature::Compute) || !m_device.Supports(Feature::IndirectDraw)) return false; // GPU 선택 결과를 간접 드로에 사용할 수 없는 장치에서는 이 경로를 활성화하지 않는다.
#if defined(ENABLE_METAL) // 컴파일 시 Metal 셰이더의 kernelMain 진입점 이름을 고른다.
    constexpr const char* entry="kernelMain";
#else // 그 외 백엔드는 main 진입점 이름을 사용한다.
    constexpr const char* entry="main";
#endif
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // 지원 백엔드 빌드에만 포함된 셰이더 바이트로 생성한다.
    if(!m_shader) m_shader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuVisibilityCompute,kGpuVisibilityComputeSize}); // 이전 시도에서 셰이더가 남아 있으면 재생성하지 않는다.
#endif
    if(!m_shader) return false; // 사용할 셰이더가 없으면 파이프라인 구성을 중단한다.
    // 입력/배치 읽기, 생존 인스턴스/간접 인자/LOD 이력 쓰기, 카메라 상수의 바인딩 번호를 셰이더와 맞춘다.
    const ResourceBindingLayout bindings[]={{0,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {1,ResourceBindingType::ReadOnlyStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {2,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {3,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {4,ResourceBindingType::ReadWriteStorageBuffer,1,ShaderStageFlags::Compute,{}},
        {5,ResourceBindingType::ConstantBuffer,1,ShaderStageFlags::Compute,{}}};
    ComputePipelineDesc desc;desc.computeShader=m_shader;
    // 한 스레드 그룹이 64항목을 처리하고 16바이트 inline 상수로 작업 종류·2차원 dispatch 폭·이력 초기화 여부를 넘긴다.
    desc.layout={bindings,6,16,ShaderStageFlags::Compute,15};desc.threadGroupSize[0]=64;
    m_pipeline=m_device.CreateComputePipeline(desc);return m_pipeline!=nullptr;
}

// GPU에 넘기기 전에 배열 크기, 설정 수치, 배치 출력 구간, LOD별 배치 참조와 최악의 출력 개수를 CPU에서 검증한다.
bool GpuVisibility::Prepare(const std::vector<Instance>& inputs,const std::vector<Batch>& batches,const Settings& settings)
{
    using namespace RHI;
    // 상태 전환 방향마다 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)의 폭은 유한한 0 이상 0.5 미만이어야 한다.
    if(inputs.empty() || batches.empty() || inputs.size()>UINT32_MAX/sizeof(Instance) || // 빈 입력·범위 초과·0 크기·비유한 값·양수가 아닌 픽셀 오차·허용 범위 밖 히스테리시스를 거부한다.
        batches.size()>UINT32_MAX/sizeof(Batch) || !settings.width || !settings.height ||
        !std::isfinite(settings.pixelError) || settings.pixelError<=0 ||
        !std::isfinite(settings.screenSize) || settings.screenSize<=0 || settings.screenSize>1 ||
        !std::isfinite(settings.hysteresis) || settings.hysteresis<0 || settings.hysteresis>=.5f) return false;
    uint64_t capacity=0;
    for(const auto& batch:batches)
    {
        if(!batch.capacity || !batch.indexCount || batch.instanceOffset<capacity) return false; // 배치는 출력 공간과 인덱스를 가져야 하고 앞 배치의 끝보다 앞에서 시작하면 안 된다.
        capacity=uint64_t(batch.instanceOffset)+batch.capacity;
    }
    if(capacity>UINT32_MAX/sizeof(VisibleInstance)) return false; // 마지막 배치 끝까지의 인스턴스 바이트 크기가 RHI 32비트 범위를 넘으면 중단한다.
    // 여러 GPU 실행이 동시에 결과를 추가해도 저장 위치가 겹치지 않게 위치를 확보하는 것(원자적 append)을 사용한다.
    // CPU에서 각 배치로 갈 수 있는 모든 원본 인스턴스를 세고, 원자적 append가 다음 배치 구간을 침범하지 않도록 확보된 용량과 비교한다.
    std::vector<uint32_t> needed(batches.size(),0);
    for(const auto& input:inputs)
    {
        if(!input.lodCount || input.lodCount>4) return false; // 원본을 포함한 LOD 개수는 1~4여야 고정 배열 안에서 접근할 수 있다.
        for(uint32_t level=0;level<input.lodCount;++level)
        {
            const auto batch=input.batches[level];
            if(batch>=batches.size()) return false; // 각 LOD가 참조하는 출력 배치가 존재하는지 먼저 확인한다.
            // 한 인스턴스의 여러 LOD가 같은 배치를 가리켜도 한 번만 출력되므로 그 배치의 필요 용량에는 한 번만 센다.
            bool repeated=false;
            for(uint32_t earlier=0;earlier<level;++earlier) repeated=repeated || input.batches[earlier]==batch;
            if(!repeated && ++needed[batch]>batches[batch].capacity) return false; // 중복 배치가 아닌 후보를 추가했을 때 예약 용량을 넘으면 GPU 기록 전에 실패시킨다.
        }
    }
    if(!PreparePipeline()) return false; // 기능 또는 파이프라인 준비가 실패하면 자원/상수 준비를 계속하지 않는다.
    if(!m_inputs || m_inputData.size()!=inputs.size() || m_batches.size()!=batches.size() || m_capacity!=capacity) // 입력 수·배치 수·출력 구간 용량이 바뀌면 관련 버퍼와 이력을 한 묶음으로 다시 만든다.
    {
        ReleaseBuffers();
        // 곱셈은 64비트에서 수행한 뒤 CreateBuffer가 한도를 검사한다. 입력·상수와 출력의 초기 상태도 서로 다르게 설정한다.
        const auto count=uint64_t(inputs.size()),batchCount=uint64_t(batches.size());
        if(!CreateBuffer(m_inputs,count*sizeof(Instance),sizeof(Instance),BufferUsage::Storage,ResourceState::CopyDestination) || // 여섯 버퍼 중 하나라도 만들지 못하면 부분 할당 전체를 정리해 일관된 미준비 상태로 돌아간다.
            !CreateBuffer(m_batchData,batchCount*sizeof(Batch),sizeof(Batch),BufferUsage::Storage,ResourceState::CopyDestination) ||
            !CreateBuffer(m_frame,sizeof(Frame),0,BufferUsage::Constant,ResourceState::CopyDestination) ||
            !CreateBuffer(m_instances,capacity*sizeof(VisibleInstance),sizeof(VisibleInstance),BufferUsage::Storage|BufferUsage::Vertex,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_arguments,batchCount*sizeof(DrawIndexedIndirectArguments),sizeof(uint32_t),BufferUsage::Storage|BufferUsage::Indirect,ResourceState::UnorderedAccess) ||
            !CreateBuffer(m_lodHistory,count*sizeof(uint32_t),sizeof(uint32_t),BufferUsage::Storage,ResourceState::UnorderedAccess))
            {ReleaseBuffers();return false;}
        // 기본 파이프라인의 여섯 바인딩을 실제 버퍼에 연결한다. 출력 버퍼는 compute 저장 용도와 드로 입력 용도를 함께 가진다.
        const ResourceBinding bindings[]={{0,0,m_inputs,nullptr,0,m_inputs->GetDesc().size,{}},
            {1,0,m_batchData,nullptr,0,m_batchData->GetDesc().size,{}},
            {2,0,m_instances,nullptr,0,m_instances->GetDesc().size,{}},
            {3,0,m_arguments,nullptr,0,m_arguments->GetDesc().size,{}},
            {4,0,m_lodHistory,nullptr,0,m_lodHistory->GetDesc().size,{}},{5,0,m_frame,nullptr,0,sizeof(Frame),{}}};
        m_resources=m_device.CreateResourceSet({m_pipeline,bindings,6});
        if(!m_resources) {ReleaseBuffers();return false;} // 바인딩 생성 실패도 버퍼 일부만 남겨 성공한 것처럼 보이지 않도록 모두 정리한다.
        m_capacity=static_cast<uint32_t>(capacity);
    }
    // 동일한 입력/배치는 CPU 사본과 GPU 업로드를 재사용하며 이번 프레임의 패스 등록 상태만 초기화한다.
    const bool sameInputs=m_inputData.size()==inputs.size() &&
        std::memcmp(m_inputData.data(),inputs.data(),inputs.size()*sizeof(Instance))==0;
    const bool sameBatches=m_batches.size()==batches.size() &&
        std::memcmp(m_batches.data(),batches.data(),batches.size()*sizeof(Batch))==0;
    m_uploadDirty[0]=m_uploadDirty[0] || !sameInputs;
    m_uploadDirty[1]=m_uploadDirty[1] || !sameBatches;
    if(!sameInputs)m_inputData=inputs;
    if(!sameBatches)m_batches=batches;
    m_pending=false;m_pendingOcclusion=false;
    m_resetHistory=m_resetHistory || settings.resetHistory;
    const auto vp=settings.camera.projection*settings.camera.view;
    // 열 우선 행렬 저장에서 각 행을 뽑아 클립 공간 부등식 -w<=x,y<=w와 0<=z<=w를 월드 공간 평면으로 바꾼다.
    const auto row=[&](uint32_t r) {return Math::float4{vp.m[r],vp.m[4+r],vp.m[8+r],vp.m[12+r]};};
    const auto x=row(0),y=row(1),z=row(2),w=row(3);
    const auto combine=[](Math::float4 a,Math::float4 b,float sign) {
        Math::float4 plane={a.x+sign*b.x,a.y+sign*b.y,a.z+sign*b.z,a.w+sign*b.w};
        const float length=std::sqrt(plane.x*plane.x+plane.y*plane.y+plane.z*plane.z);
        // 평면의 앞 세 계수는 표면에 수직인 방향(법선)을 나타낸다.
        if(!std::isfinite(length) || length<=1e-20f || !std::isfinite(plane.w)) return Math::float4{0,0,0,0}; // 법선 길이나 평면 상수가 비유한 값이거나 길이가 1e-20 이하이면 0 평면을 반환해 잘못된 컬링을 막는다.
        return Math::float4{plane.x/length,plane.y/length,plane.z/length,plane.w/length};
    };
    const auto expand=[&](float amount) {return Math::float4{w.x*amount,w.y*amount,w.z*amount,w.w*amount};};
    // 좌우·상하 클립 경계를 한 픽셀만큼 넓힌다. 비지터 카메라 경계 밖이던 물체가 지터로 화면에 들어올 가능성을 남긴다.
    const auto horizontal=expand(1.f+2.f/settings.width),vertical=expand(1.f+2.f/settings.height);
    const auto previousFrame=m_frameData;
    // 좌/우/아래/위는 확장 w±x,y, near는 z, far는 w-z다. 정규화된 평면과 월드 구 반지름으로 셰이더가 밖에 있는지 검사한다.
    m_frameData.planes={combine(horizontal,x,1),combine(horizontal,x,-1),combine(vertical,y,1),combine(vertical,y,-1),
        combine(z,{0,0,0,0},1),combine(w,z,-1)};
    const auto& view=settings.camera.view;const auto& projection=settings.camera.projection;
    // 카메라 view의 -Z 행으로 양수 전방 깊이를 계산한다. LOD는 이 깊이와 월드 구의 가까운 면을 사용해 화면 오차를 추정한다.
    m_frameData.depthRow={-view.m[2],-view.m[6],-view.m[10],-view.m[14]};
    // 가로/세로 중 큰 픽셀 투영 배율, 허용 오차, 히스테리시스, 원근 여부를 한 float4로 넘긴다.
    m_frameData.projection={std::max(std::abs(projection.m[0])*settings.width,std::abs(projection.m[5])*settings.height)*.5f,
        settings.pixelError,settings.hysteresis,std::abs(projection.m[15])<1e-6f ? 1.f : 0.f};
    const float screenSize=settings.screenSize*std::min(settings.width,settings.height);
    m_frameData.lodScreenSizes={screenSize,screenSize*.5f,screenSize*.25f,0};
    m_frameData.counts={static_cast<uint32_t>(inputs.size()),static_cast<uint32_t>(batches.size()),settings.frustum?1u:0u,settings.lod?1u:0u};
    m_uploadDirty[2]=m_uploadDirty[2] || std::memcmp(&previousFrame,&m_frameData,sizeof(Frame))!=0;
    m_possibleBatches.assign(batches.size(),settings.frustum?0u:1u);
    if(settings.frustum)
        for(const auto& input:inputs)
        {
            if(DefinitelyOutside(input,m_frameData.planes))continue;
            for(uint32_t level=0;level<input.lodCount;++level)m_possibleBatches[input.batches[level]]=1;
        }
    return true;
}

// 입력 인스턴스·배치·프레임 상수 세 버퍼의 업로드와 상태 전환을 commands에 기록한다. 생존 출력·간접 인자·LOD 이력은 이후 compute가 쓰며, 이 함수는 명령을 제출하거나 GPU 완료를 기다리지 않는다.
bool GpuVisibility::RecordUploads(RHI::ICommandList& commands)
{
    using namespace RHI;
    if(!m_resources || m_inputData.empty()) return false; // Prepare의 입력과 바인딩이 없으면 올릴 자료가 없으므로 실패한다.
    const BufferHandle buffers[]={m_inputs,m_batchData,m_frame};
    const ResourceState states[]={ResourceState::ShaderResource,ResourceState::ShaderResource,ResourceState::ConstantBuffer};
    const void* data[]={m_inputData.data(),m_batches.data(),&m_frameData};
    const uint32_t sizes[]={static_cast<uint32_t>(m_inputData.size()*sizeof(Instance)),
        static_cast<uint32_t>(m_batches.size()*sizeof(Batch)),sizeof(Frame)};
    for(uint32_t i=0;i<3;++i)
    {
        if(m_uploadReady && !m_uploadDirty[i])continue;
        if(m_uploadReady) {const ResourceBarrierDesc barrier={buffers[i],nullptr,states[i],ResourceState::CopyDestination,{}};commands.ResourceBarrier(&barrier,1);} // 이전에 업로드 제출한 버퍼는 읽기 상태에서 CopyDestination으로 되돌린다. 최초 생성은 이미 복사 대상이다.
        if(!m_device.UpdateBuffer(commands,buffers[i],0,data[i],sizes[i])) return false; // 어느 입력 슬롯에서든 복사 기록이 실패하면 호출자에게 중단을 알린다.
        const ResourceBarrierDesc barrier={buffers[i],nullptr,ResourceState::CopyDestination,states[i],{}};
        commands.ResourceBarrier(&barrier,1);
    }
    return true;
}

// 초기화 패스와 선택 패스를 순서대로 등록한다. GPU의 선택 결과는 VertexBuffer/IndirectArgument 최종 상태로 그래프에 선언한다.
// 선택 셰이더는 평면까지의 부호 거리 < -구 반지름-수치 여유일 때만 제거한다. 경계 비트가 없거나 구가 비정상이면 원본 LOD로 남겨 잘못된 제거를 피한다.
bool GpuVisibility::AddPasses(RHI::RenderGraph& graph)
{
    using namespace RHI;
    if(m_pending || !m_resources || m_inputData.empty()) return false; // 같은 준비 상태에서 중복 패스를 등록하거나 준비되지 않은 입력으로 실행하지 못하게 한다.
    const auto inputs=graph.ImportBuffer("GPU visibility inputs",m_inputs,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto batches=graph.ImportBuffer("GPU visibility batches",m_batchData,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto frame=graph.ImportBuffer("GPU visibility camera",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto history=graph.ImportBuffer("GPU LOD history",m_lodHistory,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
    // 지난 프레임 제출이 확정되었으면 드로용 상태에서, 처음 만든 버퍼면 UnorderedAccess 상태에서 시작한다.
    m_instancesGraph=graph.ImportBuffer("GPU visible instances",m_instances,m_ready?ResourceState::VertexBuffer:ResourceState::UnorderedAccess,ResourceState::VertexBuffer);
    m_argumentsGraph=graph.ImportBuffer("GPU indirect arguments",m_arguments,m_ready?ResourceState::IndirectArgument:ResourceState::UnorderedAccess,ResourceState::IndirectArgument);
    if(!inputs.IsValid() || !batches.IsValid() || !frame.IsValid() || !history.IsValid() || // 필요한 그래프 자원 중 하나라도 등록 실패면 선택 패스를 구성하지 않는다.
        !m_instancesGraph.IsValid() || !m_argumentsGraph.IsValid()) return false;
    const auto pipeline=m_pipeline;const auto resources=m_resources;
    // Preparation is restricted to stock instance streams. Unsupported devices
    // retain zero firstInstance and select each batch through its VB offset.
    const uint32_t baseInstance=m_device.Supports(Feature::IndirectFirstInstance)?1u:0u;
    for(uint32_t operation=0;operation<2;++operation)
    {
        // operation 0은 배치별 간접 인스턴스 수를 0으로 만들고 필요한 LOD 이력을 초기화한다. operation 1은 구 컬링·LOD 선택 후 생존 항목을 배치별로 append한다.
        auto& pass=graph.AddPass(operation?"GPU frustum and LOD selection":"GPU indirect reset", RHI::RGRecordingMode::Independent);
        // 리소스 읽기/쓰기 의존성을 선언해 초기화→선택 및 이후 드로 사이의 메모리 순서와 상태 전환을 그래프가 구성하게 한다.
        pass.Read(inputs,ResourceState::ShaderResource).Read(batches,ResourceState::ShaderResource).Read(frame,ResourceState::ConstantBuffer)
            .Read(history,ResourceState::UnorderedAccess).Write(history,ResourceState::UnorderedAccess)
            .Write(m_instancesGraph,ResourceState::UnorderedAccess).Write(m_argumentsGraph,ResourceState::UnorderedAccess).SetPipeline(pipeline);
        if(operation) pass.Read(m_argumentsGraph,ResourceState::UnorderedAccess); // 선택 패스는 앞에서 초기화한 인자를 원자적으로 증가시키므로 쓰기뿐 아니라 읽기도 선언한다.
        const auto count=operation?m_frameData.counts[0]:std::max(m_frameData.counts[0],m_frameData.counts[1]);
        // 64개 단위로 그룹 수를 올림하고 X축 65,535그룹을 넘는 나머지는 Y축으로 펼친다. 셰이더는 전달된 행 폭으로 전역 항목 번호를 복원한다.
        const uint32_t groups=count/64+(count%64!=0),groupsX=std::min(groups,65535u),groupsY=groups/groupsX+(groups%groupsX!=0);
        // 이력 초기화 요청과 장치가 지원하는 firstInstance 모드를 상수로 전달한다.
        const std::array<uint32_t,4> push={operation,groupsX*64,m_resetHistory?1u:0u,baseInstance};
        // 지연 실행에 필요한 핸들·상수·그룹 크기는 값으로 캡처한다. 자원 실물은 그래프 실행 동안 유지되어야 한다.
        pass.SetExecute([pipeline,resources,push,groupsX,groupsY](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(push),push.data());commands->Dispatch(groupsX,groupsY,1);
        });
    }
    // 패스 등록이 완료되었다는 표시이며 GPU 실행 완료가 아니다. 성공 제출 후 CommitFrame이 다음 프레임 상태를 확정한다.
    m_pending=true;return true;
}

// 가림 전용 바인딩과 소유 HiZ·상수 버퍼만 정리한다. m_hizDepth는 외부 깊이를 기억하는 핸들이므로 null로 끊기만 한다.
void GpuVisibility::ReleaseOcclusion()
{
    m_device.DestroyResourceSet(m_occlusionResources);m_occlusionResources=nullptr;
    for(auto resources:m_hizResources) m_device.DestroyResourceSet(resources);
    m_hizResources.clear();
    m_device.DestroyTexture(m_hiz);m_hiz=nullptr;m_hizDepth=nullptr;
    m_device.DestroyBuffer(m_occlusionFrame);m_occlusionFrame=nullptr;
    m_hizReady=false;m_occlusionReady=false;m_pendingOcclusion=false;
}

bool GpuVisibility::SupportsOcclusion(const RHI::IDevice& device)
{
    const OcclusionLayouts layouts;
    // HiZ needs one previous mip and two output storage views in the same dispatch.
    return device.Supports(RHI::Feature::Compute) && device.Supports(RHI::Feature::IndirectDraw) &&
        device.Supports(layouts.HiZ()) && device.Supports(layouts.Occlusion());
}

// 현재 깊이의 HiZ와 가림 선택 파이프라인을 준비하고 기본 가시성 버퍼를 공유한다.
bool GpuVisibility::PrepareOcclusion(RHI::TextureHandle depth)
{
    using namespace RHI;
    if(!depth || !m_resources || !SupportsOcclusion(m_device)) return false;
    const OcclusionLayouts layouts;
#if defined(ENABLE_METAL) // Metal용 compute 진입점을 선택하는 컴파일 시 분기다.
    constexpr const char* entry="kernelMain";
#else // 그 외 빌드에서는 main 진입점을 쓴다.
    constexpr const char* entry="main";
#endif
    if(!m_hizPipeline) // HiZ 축소 파이프라인이 없을 때만 생성한다.
    {
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // 지원 백엔드 빌드에 들어 있는 HiZ 셰이더 바이트를 사용한다.
        if(!m_hizShader) m_hizShader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuHiZCompute,kGpuHiZComputeSize}); // 아직 없는 HiZ 셰이더만 생성한다.
#endif
        if(!m_hizShader) return false; // HiZ 셰이더를 얻지 못하면 깊이 피라미드를 만들 수 없다.
        ComputePipelineDesc desc;desc.computeShader=m_hizShader;
        desc.layout=layouts.HiZ();desc.threadGroupSize[0]=desc.threadGroupSize[1]=8;
        m_hizPipeline=m_device.CreateComputePipeline(desc);
        if(!m_hizPipeline) return false; // HiZ 파이프라인 생성 실패를 가림 준비 실패로 전달한다.
    }
    if(!m_occlusionPipeline) // 기본 가시성 입력에 HiZ와 지터 카메라를 추가로 읽는 가림 선택 파이프라인을 최초 구성한다.
    {
#if defined(ENABLE_METAL) || defined(ENABLE_D3D12) || defined(ENABLE_VULKAN) // 지원 백엔드의 가림 변형 셰이더를 포함한 경우에만 생성한다.
        if(!m_occlusionShader) m_occlusionShader=m_device.CreateShader({ShaderStage::Compute,entry,kGpuOcclusionCompute,kGpuOcclusionComputeSize}); // 없는 가림 compute 셰이더만 만든다.
#endif
        if(!m_occlusionShader) return false; // 가림 셰이더가 없으면 사용할 수 없는 결과를 성공으로 표시하지 않는다.
        ComputePipelineDesc desc;desc.computeShader=m_occlusionShader;
        desc.layout=layouts.Occlusion();desc.threadGroupSize[0]=64;
        m_occlusionPipeline=m_device.CreateComputePipeline(desc);
        if(!m_occlusionPipeline) return false; // 가림 파이프라인 생성 실패 시 자원 구성 단계를 중단한다.
    }
    if(m_hizDepth==depth && m_hiz && m_occlusionResources) return true; // 같은 깊이 핸들에 필요한 자원이 모두 남아 있으면 할당을 재사용한다. HiZ 픽셀 내용을 이전 프레임 값으로 재사용하는 뜻은 아니다.
    ReleaseOcclusion();
    const auto& source=depth->GetDesc();
    // 원본 깊이 4x4 블록을 한 셀로 줄이는 첫 레벨 크기를 올림 계산한다. 화면의 남는 끝 픽셀도 버리지 않는다.
    uint32_t width=source.width/4+(source.width%4!=0),height=source.height/4+(source.height%4!=0);
    // 첫 4×4 레벨을 2의 거듭제곱 크기로 확장해 이후 native mip의 끝 영역 누락을 막으며, 추가 여백은 미피복 깊이로 채운다.
    const auto padded=[](uint32_t value) {uint64_t result=1;while(result<value) result*=2;return result;};
    const auto paddedWidth=padded(width),paddedHeight=padded(height);
    if(paddedWidth>m_device.GetLimit(Limit::Texture2DDimension) || paddedHeight>m_device.GetLimit(Limit::Texture2DDimension)) return false; // 패딩 이후 크기까지 장치 텍스처 한도에 들어야 한다.
    TextureDesc desc;desc.width=static_cast<uint32_t>(paddedWidth);desc.height=static_cast<uint32_t>(paddedHeight);
    // 큰 축이 1이 될 때까지 mip 개수를 늘린다. 각 단계는 R32_FLOAT 하나의 최대 깊이를 저장한다.
    for(uint32_t size=std::max(desc.width,desc.height);size>1;size/=2) ++desc.mipLevels;
    desc.format=Format::R32_FLOAT;desc.usage=TextureUsage::Storage|TextureUsage::ShaderResource;
    m_hiz=m_device.CreateTexture(desc);
    if(!m_hiz || !CreateBuffer(m_occlusionFrame,sizeof(OcclusionFrame),0,BufferUsage::Constant,ResourceState::CopyDestination)) // HiZ 또는 가림 카메라 상수 버퍼 생성 실패 시 가림 전용 부분 자원을 모두 정리한다.
        {ReleaseOcclusion();return false;}
    assert(desc.width && desc.height && (desc.width&(desc.width-1))==0 && (desc.height&(desc.height-1))==0);
    for(uint32_t level=0;level<desc.mipLevels;level+=2)
    {
        // A dispatch writes a pair of mips. The odd tail binds its valid output
        // twice, but operation.z=1 prevents every access through the second view.
        const TextureSubresourceRange previous={level?level-1:0,1,0,1},current={level,1,0,1};
        const TextureSubresourceRange next={std::min(level+1,desc.mipLevels-1),1,0,1};
        const ResourceBinding bindings[]={{0,0,nullptr,depth,0,0,{0,1,0,1}},
            {1,0,nullptr,m_hiz,0,0,previous},{2,0,nullptr,m_hiz,0,0,current},{3,0,nullptr,m_hiz,0,0,next}};
        auto resources=m_device.CreateResourceSet({m_hizPipeline,bindings,4});
        if(!resources) {ReleaseOcclusion();return false;} // 한 mip 바인딩이라도 실패하면 완성되지 않은 피라미드 묶음을 폐기한다.
        try {m_hizResources.push_back(resources);}
        // 벡터 확장 예외가 나면 아직 소유 목록에 들어가지 못한 방금 생성한 바인딩을 직접 파괴한 뒤 예외를 전달한다.
        catch(...) {m_device.DestroyResourceSet(resources);throw;}
    }
    // 기본 입력/배치/출력/이력과 함께 HiZ·가림 카메라 상수를 연결해 같은 원본 목록을 가림 조건까지 포함하여 다시 선택한다.
    const ResourceBinding bindings[]={{0,0,m_inputs,nullptr,0,m_inputs->GetDesc().size,{}},
        {1,0,m_batchData,nullptr,0,m_batchData->GetDesc().size,{}},
        {2,0,m_instances,nullptr,0,m_instances->GetDesc().size,{}},
        {3,0,m_arguments,nullptr,0,m_arguments->GetDesc().size,{}},
        {4,0,m_lodHistory,nullptr,0,m_lodHistory->GetDesc().size,{}},{5,0,m_frame,nullptr,0,sizeof(Frame),{}},
        {6,0,nullptr,m_hiz,0,0,{}},{7,0,m_occlusionFrame,nullptr,0,sizeof(OcclusionFrame),{}}};
    m_occlusionResources=m_device.CreateResourceSet({m_occlusionPipeline,bindings,8});
    if(!m_occlusionResources) {ReleaseOcclusion();return false;} // 최종 가림 바인딩 생성 실패 시 가림 자원을 정리하고 실패한다.
    m_hizDepth=depth;return true;
}

// 이번 프레임 깊이를 읽는 HiZ 생성 뒤 간접 인자 재초기화와 가림 선택을 추가한다. 이전 프레임 깊이로 현재 장면을 추정하는 경로가 아니다.
// 가림 셰이더는 구를 둘러싼 상자의 여덟 꼭짓점을 화면에 투영하고 그 사각형을 덮는 HiZ 셀을 모두 읽는다.
// 최대 차폐 깊이+여유가 객체의 가장 가까운 깊이보다 앞일 때만 제거한다. near 평면 교차·비정상 값·미피복 깊이 등 확신할 수 없는 경우는 남긴다.
bool GpuVisibility::AddOcclusionPasses(RHI::RenderGraph& graph,RHI::TextureHandle depth,
    RHI::RGResourceHandle depthHandle,const Camera& jitteredCamera)
{
    using namespace RHI;
    if(!m_pending || m_pendingOcclusion || !depth || !depthHandle.IsValid() || !PrepareOcclusion(depth)) return false; // 기본 선택 패스가 없거나 이미 가림을 등록했거나 깊이/핸들/전용 자원이 유효하지 않으면 추가하지 않는다.
    const auto& depthDesc=depth->GetDesc();const auto& hizDesc=m_hiz->GetDesc();
    OcclusionFrame constants;
    // 깊이를 실제로 그린 지터 카메라를 사용해 화면 경계를 맞춘다. viewport.z는 깊이 여유 0.0001, w는 화면 사각형 확장 2픽셀이다.
    constants.viewProjection=jitteredCamera.projection*jitteredCamera.view;
    constants.viewport={float(depthDesc.width),float(depthDesc.height),.0001f,2.f};
    constants.info={hizDesc.mipLevels,0,0,0};
    const auto camera=graph.ImportBuffer("GPU occlusion camera",m_occlusionFrame,
        m_occlusionReady?ResourceState::ConstantBuffer:ResourceState::CopyDestination,ResourceState::ConstantBuffer);
    // HiZ 텍스처 할당은 유지하되 이번 프레임에 모든 mip를 다시 쓰는 그래프 자원으로 등록한다. ready는 시작 상태를 고르기 위한 정보다.
    const auto hiz=graph.ImportTexture("GPU current HiZ",m_hiz,
        m_hizReady?ResourceState::ShaderResource:ResourceState::Undefined,ResourceState::ShaderResource);
    const auto inputs=graph.ImportBuffer("GPU visibility inputs",m_inputs,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto batches=graph.ImportBuffer("GPU visibility batches",m_batchData,ResourceState::ShaderResource,ResourceState::ShaderResource);
    const auto frame=graph.ImportBuffer("GPU visibility camera",m_frame,ResourceState::ConstantBuffer,ResourceState::ConstantBuffer);
    const auto history=graph.ImportBuffer("GPU LOD history",m_lodHistory,ResourceState::UnorderedAccess,ResourceState::UnorderedAccess);
    if(!camera.IsValid() || !hiz.IsValid() || !inputs.IsValid() || !batches.IsValid() || !frame.IsValid() || !history.IsValid()) return false; // HiZ·카메라·기본 입력의 그래프 등록 중 하나라도 실패하면 가림 패스 구성을 중단한다.
    const auto constantBuffer=m_occlusionFrame;auto* device=&m_device;
    // 카메라 상수를 값으로 캡처하고 그래프가 패스 콜백을 호출할 때 업로드 명령을 기록한다. 기록 실패는 예외로 전달하여 그래프 실행을 실패 처리하며, 콜백 호출 자체는 GPU 복사 완료를 뜻하지 않는다.
    graph.AddPass("GPU occlusion camera upload", RHI::RGRecordingMode::Independent).Write(camera,ResourceState::CopyDestination)
        .SetExecute([device,constantBuffer,constants](ICommandList* commands) {
            if(!device->UpdateBuffer(*commands,constantBuffer,0,&constants,sizeof(constants))) // 가림 상수를 기록하지 못한 상태로 compute가 진행하지 않도록 실행 실패를 발생시킨다.
                throw std::runtime_error("GPU occlusion camera upload failed");
        });
    assert(m_hizResources.size()==(hizDesc.mipLevels+1)/2);
    for(uint32_t level=0;level<hizDesc.mipLevels;level+=2)
    {
        // Keep a whole-HiZ UAV dependency between dispatches; only the second
        // output inside each workgroup is reduced directly from shared memory.
        auto& pass=graph.AddPass(level?"GPU HiZ max reduction":"GPU HiZ depth reduction", RHI::RGRecordingMode::Independent);
        pass.Read(depthHandle,ResourceState::ShaderResource).Write(hiz,ResourceState::UnorderedAccess).SetPipeline(m_hizPipeline);
        if(level) pass.Read(hiz,ResourceState::UnorderedAccess); // 두 번째 mip부터는 앞 mip의 결과를 읽으므로 같은 HiZ의 읽기 의존성도 선언한다.
        const uint32_t width=std::max(1u,hizDesc.width>>level),height=std::max(1u,hizDesc.height>>level);
        const uint32_t outputCount=std::min(2u,hizDesc.mipLevels-level);
        const uint32_t groupsX=width/8+(width%8!=0),groupsY=height/8+(height%8!=0);
        assert(outputCount==1 || (groupsX*4>=std::max(1u,width/2) && groupsY*4>=std::max(1u,height/2)));
        // x/y retain the original footprint/input mode; z selects one or two outputs.
        const std::array<uint32_t,4> operation={level?2u:4u,level?1u:0u,outputCount,0};
        const auto pipeline=m_hizPipeline;const auto resources=m_hizResources[level/2];
        pass.SetExecute([pipeline,resources,operation,groupsX,groupsY](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            // 8x8 그룹 크기로 출력 셀을 모두 덮도록 올림 dispatch한다. 가장자리의 초과 스레드는 셰이더가 범위를 검사한다.
            commands->SetInlineConstants(0,sizeof(operation),operation.data());commands->Dispatch(groupsX,groupsY,1);
        });
    }
    const uint32_t baseInstance=m_device.Supports(Feature::IndirectFirstInstance)?1u:0u;
    for(uint32_t operation=0;operation<2;++operation)
    {
        // 이전 기본 선택 결과에 덧붙이지 않도록 간접 인스턴스 수를 다시 0으로 만든 후, 원본 입력에 절두체·LOD·가림 조건을 적용해 출력 목록을 다시 채운다.
        auto& pass=graph.AddPass(operation?"GPU HiZ occlusion selection":"GPU occlusion indirect reset", RHI::RGRecordingMode::Independent);
        pass.Read(inputs,ResourceState::ShaderResource).Read(batches,ResourceState::ShaderResource).Read(frame,ResourceState::ConstantBuffer)
            .Read(camera,ResourceState::ConstantBuffer).Read(hiz,ResourceState::ShaderResource)
            .Read(history,ResourceState::UnorderedAccess).Write(history,ResourceState::UnorderedAccess)
            .Write(m_instancesGraph,ResourceState::UnorderedAccess).Write(m_argumentsGraph,ResourceState::UnorderedAccess).SetPipeline(m_occlusionPipeline);
        if(operation) pass.Read(m_argumentsGraph,ResourceState::UnorderedAccess); // 가림 선택도 초기화된 간접 카운터를 원자적으로 증가시키므로 읽기/쓰기 관계를 선언한다.
        const auto count=operation?m_frameData.counts[0]:std::max(m_frameData.counts[0],m_frameData.counts[1]);
        // 기본 선택과 동일하게 64항목 그룹을 2차원으로 펼쳐 입력/배치 초기화 전체를 덮는다.
        const uint32_t groups=count/64+(count%64!=0),groupsX=std::min(groups,65535u),groupsY=groups/groupsX+(groups%groupsX!=0);
        // 앞 기본 선택에서 갱신한 LOD 이력은 유지하고, 장치가 지원하는 firstInstance 모드를 전달한다.
        const std::array<uint32_t,4> push={operation,groupsX*64,0,baseInstance};
        const auto pipeline=m_occlusionPipeline;const auto resources=m_occlusionResources;
        pass.SetExecute([pipeline,resources,push,groupsX,groupsY](ICommandList* commands) {
            commands->BindComputePipeline(pipeline);commands->BindResourceSet(resources);
            commands->SetInlineConstants(0,sizeof(push),push.data());commands->Dispatch(groupsX,groupsY,1);
        });
    }
    // 가림 패스가 등록되었다는 표시다. 실제 GPU 완료나 제거 개수는 이 bool로 알 수 없다.
    m_pendingOcclusion=true;return true;
}

// 성공 제출 후 다음 프레임이 올바른 시작 상태를 사용하도록 플래그를 확정한다. 실제 완료 확인은 장치 펜스 등 별도 동기화의 역할이다.
void GpuVisibility::CommitFrame()
{
    if(!m_pending) return; // 이번 준비에서 기본 패스를 등록한 적이 없으면 확정할 상태가 없다.
    if(m_pendingOcclusion) {m_hizReady=true;m_occlusionReady=true;m_pendingOcclusion=false;} // 가림도 등록한 프레임이면 HiZ와 카메라 상수의 다음 시작 상태를 읽기 상태로 기억한다.
    // 출력의 드로용 최종 상태를 기억하고 LOD 이력 초기화 요청을 소비한다. 다음 프레임은 같은 입력 순서의 이력을 이어 쓸 수 있다.
    m_ready=true;m_resetHistory=false;m_pending=false;
}
}
