#pragma once

#include "dyf/Camera.h"
#include "dyf/RHI/Buffer.h"
#include "dyf/RHI/RenderGraph.h"
#include <array>
#include <vector>

namespace dyf
{
class GpuVisibility
{
public:
    // 한 원본 인스턴스의 변환·경계 구·LOD 오차·LOD별 배치 번호를 셰이더와 같은 메모리 배치로 전달한다.
    struct Instance
    {
        Math::float4x4 model={};
        Math::float4x4 previousMVP={}; // 이전 프레임 변환도 출력에 이어 보내 속도/시간 누적 경로가 사용할 수 있게 한다.
        Math::float4 centerRadius={}; // xyz는 월드 경계 구 중심, w는 반지름이다. flags와 유한성 검사로 경계 사용 가능 여부를 판단한다.
        // 원본 오차는 0이며 간소화 단계의 오차는 월드 단위다.
        Math::float4 lodErrors={};
        std::array<uint32_t,4> batches={}; // LOD 0~3에서 선택할 배치 인덱스다. 각 값은 Prepare에서 배치 배열 범위 안인지 확인한다.
        uint32_t lodCount=1,flags=1,padding0=0,padding1=0; // 원본 포함 단계 수와 경계 유효 비트를 전달하고 뒤 두 값은 GPU 레이아웃 정렬용이다.
    };
    // 살아남은 항목은 드로용으로 변환 두 개만 복사한다. 경계·오차·선택 정보는 정점 입력에 필요하지 않다.
    struct VisibleInstance
    {
        Math::float4x4 model,previousMVP;
    };
    // 같은 인덱스 드로를 공유하는 출력 구간이다. indexCount/firstIndex/vertexOffset은 간접 드로 인자에 들어간다.
    struct Batch
    {
        uint32_t indexCount=0,firstIndex=0;
        int32_t vertexOffset=0;
        // IndirectFirstInstance 지원 시 시작 행을 인수에 넣고 VB는 offset 0에 바인딩한다.
        // 미지원 시 firstInstance 0과 이 시작점의 VB 바이트 오프셋을 사용한다.
        uint32_t instanceOffset=0,capacity=0; // 출력 배열의 배치별 시작과 최대 개수다. 실제 GPU 가시 개수는 아니며 배치 구간은 겹치면 안 된다. // 출력 인스턴스 배열에서 이 배치가 차지할 시작과 최대 개수다. 배치 구간은 서로 겹치면 안 된다.
        std::array<uint32_t,3> padding={};
    };
    // 지터 가장자리 여유를 포함한 절두체와 화면 픽셀 오차를 계산할 CPU 입력이다. 깊이 가림에는 실제 깊이를 그린 지터 카메라를 별도로 받는다.
    struct Settings
    {
        Camera camera;
        uint32_t width=0,height=0;
        float pixelError=1,screenSize=0.5f,hysteresis=0.1f;
        bool frustum=true,lod=true,resetHistory=false; // 각 검사 사용 여부와 인스턴스 순서 변경 시 LOD 이력 초기화 요청이다.
    };

    // 장치를 빌려 모든 자원을 만든다. 이 객체의 소멸과 그래프 실행이 끝날 때까지 장치와 필요한 자원이 유효해야 한다.
    explicit GpuVisibility(RHI::IDevice& device):m_device(device) {}
    ~GpuVisibility();
    GpuVisibility(const GpuVisibility&)=delete;
    GpuVisibility& operator=(const GpuVisibility&)=delete;
    // 입력 순서가 달라지면 resetHistory를 켜야 이전 객체의 LOD를 재사용하지 않는다.
    // flags의 비트 0이 없으면 경계를 알 수 없는 객체로 취급하여 제거하지 않는다.
    // 입력의 LOD 수·배치 번호·배치 구간 겹침·출력 용량을 검사하고 GPU 버퍼와 파이프라인을 확보한 뒤 CPU 프레임 상수를 만든다. GPU 선택 명령은 여기서 실행하지 않는다.
    bool Prepare(const std::vector<Instance>&,const std::vector<Batch>&,const Settings&);
    // Prepare가 보관한 CPU 입력을 명령 목록에 복사 기록한다. 성공적인 제출 뒤 호출자가 CommitUploads를 부른다.
    bool RecordUploads(RHI::ICommandList&);
    void CommitUploads() {m_uploadReady=true;m_uploadDirty.fill(false);} // 입력 제출을 확정하고 dirty 표시를 비운다. GPU 완료 펜스는 아니다.
    // 간접 인수 초기화와 절두체/LOD 선택 패스를 등록한다. 호출 자체가 GPU 계산을 실행하지는 않는다.
    bool AddPasses(RHI::RenderGraph&);
    // Texture/sampler compute layouts are optional even when buffer Compute is supported.
    static bool SupportsOcclusion(const RHI::IDevice&);
    // 현재 깊이로 HiZ를 만들고 가림을 반영한 간접 목록을 구성하는 패스를 등록한다.
    bool AddOcclusionPasses(RHI::RenderGraph&,RHI::TextureHandle depth,
        RHI::RGResourceHandle depthHandle,const Camera& jitteredCamera);
    // 그래프 제출이 성공한 프레임의 출력 상태를 확정하고 다음 프레임의 초기 상태/LOD 이력을 이어 간다.
    void CommitFrame();

    // Get 계열은 객체가 소유한 GPU 버퍼와 현재 그래프 연결 핸들을 빌려준다. CPU에서 보이는 인스턴스 개수를 읽는 API는 아니다.
    RHI::BufferHandle GetInstanceBuffer() const {return m_instances;}
    RHI::BufferHandle GetArgumentsBuffer() const {return m_arguments;}
    RHI::RGResourceHandle GetInstancesHandle() const {return m_instancesGraph;}
    RHI::RGResourceHandle GetArgumentsHandle() const {return m_argumentsGraph;}
    uint32_t GetBatchCount() const {return static_cast<uint32_t>(m_batches.size());} // CPU가 준비한 배치 수이며 컬링 후 실제 생존 인스턴스 수와 다르다.
    const Batch& GetBatch(uint32_t index) const {return m_batches[index];}
    // False proves that the unchanged GPU frustum test cannot append an instance.
    // Unknown/invalid bounds stay visible; this does not predict occlusion or LOD.
    bool MayDrawBatch(uint32_t index) const {return index>=m_possibleBatches.size() || m_possibleBatches[index]!=0;}

private:
    // 상태 전환 방향마다 서로 다른 기준을 적용해 잦은 전환을 막는 방식(히스테리시스)의 폭을 GPU에 전달한다.
    // 절두체 평면 6개·시점 깊이 계산용 행·픽셀 투영 배율과 오차·히스테리시스 폭·개수와 토글을 프레임 상수로 묶는다.
    struct Frame
    {
        std::array<Math::float4,6> planes={};
        Math::float4 depthRow={};
        Math::float4 projection={};
        Math::float4 lodScreenSizes={}; // LOD1~3을 허용하는 최대 화면 지름(픽셀)이다.
        std::array<uint32_t,4> counts={};
    };
    // 실제 깊이와 정렬되는 지터 view-projection 및 viewport·깊이 여유·화면 여유·HiZ 단계 수를 별도 전달한다.
    struct OcclusionFrame
    {
        Math::float4x4 viewProjection;
        Math::float4 viewport;
        std::array<uint32_t,4> info;
    };
    bool PreparePipeline();
    bool PrepareOcclusion(RHI::TextureHandle);
    void ReleaseOcclusion();
    void ReleaseBuffers();
    bool CreateBuffer(RHI::BufferHandle&,uint64_t bytes,uint32_t stride,RHI::BufferUsage,RHI::ResourceState);
    RHI::IDevice& m_device;
    RHI::ShaderHandle m_shader=nullptr;
    RHI::PipelineHandle m_pipeline=nullptr;
    RHI::ResourceSetHandle m_resources=nullptr;
    RHI::ShaderHandle m_hizShader=nullptr,m_occlusionShader=nullptr;
    RHI::PipelineHandle m_hizPipeline=nullptr,m_occlusionPipeline=nullptr;
    RHI::ResourceSetHandle m_occlusionResources=nullptr;
    std::vector<RHI::ResourceSetHandle> m_hizResources; // One set for each output pair (mips 0/1, 2/3, ...).
    RHI::TextureHandle m_hiz=nullptr,m_hizDepth=nullptr; // m_hiz는 소유 깊이 피라미드이며 m_hizDepth는 연결된 외부 깊이 핸들이다. 외부 깊이는 파괴하지 않는다.
    RHI::BufferHandle m_occlusionFrame=nullptr;
    RHI::BufferHandle m_inputs=nullptr,m_batchData=nullptr,m_frame=nullptr;
    RHI::BufferHandle m_instances=nullptr,m_arguments=nullptr,m_lodHistory=nullptr;
    RHI::RGResourceHandle m_instancesGraph,m_argumentsGraph;
    std::vector<Instance> m_inputData; // 원본 입력의 CPU 사본을 소유해 업로드 기록 시 호출자 벡터 수명에 의존하지 않는다.
    std::vector<Batch> m_batches;
    std::vector<uint8_t> m_possibleBatches;
    Frame m_frameData;
    uint32_t m_capacity=0; // 모든 배치 구간을 담는 출력 인스턴스 버퍼 용량이다. GPU가 현재 사용한 길이가 아니다.
    bool m_hizReady=false,m_occlusionReady=false,m_pendingOcclusion=false; // HiZ/가림 상수의 직전 제출 상태와 이번 프레임 가림 패스 등록 여부를 구분한다.
    bool m_uploadReady=false,m_ready=false,m_resetHistory=true,m_pending=false; // 입력 제출·출력 제출·이력 초기화 요구·이번 기본 패스 등록 상태를 각각 추적한다.
    std::array<bool,3> m_uploadDirty={true,true,true};
};
// C++ 구조체 크기가 셰이더 저장 버퍼의 stride와 어긋나면 컴파일 단계에서 드러나게 한다.
static_assert(sizeof(GpuVisibility::Instance)==192);
static_assert(sizeof(GpuVisibility::VisibleInstance)==128);
static_assert(sizeof(GpuVisibility::Batch)==32);
}
