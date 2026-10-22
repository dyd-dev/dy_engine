#pragma once

#include "dyf/RenderExtension.h"

namespace dyf
{
// 환경 모듈을 장착하지 않은 기본 PBR 경로의 중립 입력만 소유한다.
class BasicEnvironmentResources
{
public:
    ~BasicEnvironmentResources();
    // Prepare는 초기값 업로드와 상태 전환을 명령 목록에 기록한다. 실제 제출 성공은 호출부가 확인한다.
    bool Prepare(RHI::IDevice&,RHI::ICommandList&);
    void CommitUploads() { m_ready=true; } // 업로드 제출 뒤 재기록을 생략하는 표시다. GPU 작업 완료를 조회하는 함수는 아니다.
    // 기본 환경 자원 묶음을 반환한다. 버퍼 핸들과 검은 텍스처를 빌려주는 것이며 소유권은 넘기지 않는다.
    RenderEnvironmentResources GetResources(RHI::TextureHandle black) const;
private:
    RHI::IDevice* m_device=nullptr; // 버퍼를 만든 장치를 기억해 같은 장치에 파괴를 요청한다.
    std::array<RHI::BufferHandle,3> m_buffers={}; // 순서: 환경 상수, 빈 프로브 데이터, 프로브 개수/매개변수.
    bool m_ready=false; // 처음에는 미제출 상태이며 CommitUploads 후에만 true가 된다.
};
}
