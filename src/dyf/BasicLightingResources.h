#pragma once

#include "LocalLightData.h"
#include "dyf/RenderExtension.h"
#include "dyf/RHI/Buffer.h"
#include <array>

namespace dyf
{
struct Camera;

// 기본 셰이더에 전체 local-light 목록만 제공한다. 공간 분류와 compute는 확장의 책임이다.
class BasicLightingResources
{
public:
    BasicLightingResources()=default;
    // 버퍼 소유권이 복제되어 두 객체가 같은 핸들을 파괴하지 않도록 복사 생성·대입을 금지한다.
    BasicLightingResources(const BasicLightingResources&)=delete;
    BasicLightingResources& operator=(const BasicLightingResources&)=delete;
    ~BasicLightingResources();

    // 업로드 기록이나 제출이 실패하면 Reset 뒤 다시 준비한다.
    // active에 따라 점광원·스폿 목록을 채우거나 비운 뒤 광원 배열·더미 목록·카메라 상수 버퍼를 기록한다. 모두 성공해야 Import가 허용된다.
    bool Prepare(RHI::IDevice&,RHI::ICommandList&,const Scene&,const Camera&,bool active);
    // 준비된 버퍼를 현재 RenderGraph에 외부 자원으로 등록하고 해당 프레임의 조명 입력에 연결한다.
    bool Import(RHI::RenderGraph&,RenderLightingResources&);
    void Reset(); // GPU 버퍼와 비교용 바이트 캐시를 모두 비워 다음 Prepare가 다시 생성하게 한다.
    // Get 계열은 소유 중인 핸들을 조회한다. 반환받은 쪽이 별도로 파괴하면 안 된다.
    RHI::BufferHandle GetLights() const {return m_buffers[0].buffer;}
    RHI::BufferHandle GetList() const {return m_buffers[1].buffer;}
    RHI::BufferHandle GetConstants() const {return m_buffers[2].buffer;}

private:
    struct Buffer
    {
        RHI::BufferHandle buffer=nullptr;
        std::vector<uint8_t> bytes; // 마지막으로 업로드를 기록한 CPU 바이트를 보관해 동일 자료의 재복사를 생략한다.
        bool ready=false; // 슬롯의 업로드 기록 여부이며 GPU 완료 펜스는 아니다.
    };
    bool Upload(RHI::ICommandList&,uint32_t slot,const void*,uint32_t bytes,
        uint32_t stride,RHI::BufferUsage,RHI::ResourceState);
    RHI::IDevice* m_device=nullptr;
    std::array<Buffer,3> m_buffers; // 광원 저장 버퍼, 사용하지 않는 목록 자리, 카메라/개수 상수 버퍼 순서다.
    LocalLightData m_localLights;
    RendererClusterConstants m_data={};
    bool m_prepared=false; // 이번 Prepare 전체가 성공했을 때만 그래프 Import를 허용한다.
};
}
