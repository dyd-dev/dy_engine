#include <dyf/RHI/IDevice.h>

#include <cstring>
#include <iostream>
#include <memory>

namespace
{
    void PrintScope()
    {
        std::cout <<
        "다중 스레드 명령 기록: 현재는 지원 조회와 미지원 단계 안내를 제공하는 예제입니다.\n"
        "AcquireCommandList로 독립 명령 목록을 여러 개 만들 수 있습니다. 공개 Feature에는 스레드 안전성 조회 항목이 없습니다.\n"
        "현재 Submit은 공통 장치 잠금 아래에서 기록된 목록을 순서대로 네이티브 명령에 다시 기록합니다.\n"
        "동일 ICommandList의 동시 기록은 지원하지 않습니다. 독립 목록의 스레드별 사용 계약과 자원 공유/수명 규칙을 검증해야 합니다.\n"
        "네이티브 명령의 병렬 기록/제출이나 스레드 수에 따른 성능 향상을 이 진단에서 보장하거나 측정하지 않습니다.\n"
        "아직 제공되지 않는 실행 API를 호출하지 않으며 GPU 결과나 성능 수치를 만들지 않습니다.\n";
    }

    void PrintFeature(const dyf::RHI::IDevice& device, const char* name, dyf::RHI::Feature feature)
    {
        std::cout << "  Supports(" << name << ") = "
            << (device.Supports(feature) ? "지원" : "미지원") << '\n';
    }
}

int main(int argc, char** argv)
{
    if(argc == 2 && std::strcmp(argv[1], "--help") == 0)
    {
        std::cout << "사용법: AdvancedMultithreadRecording [--help]\n"
            "인수 없이 실행하면 RHI 장치를 생성하고 관련 기능을 조회합니다.\n"
            "--help는 GPU 없이 실행됩니다. 종료 코드: 도움말 0, 장치 생성 실패 1, 잘못된 인수 2, 예제 미지원 77.\n";
        PrintScope();
        return 0;
    }
    if(argc != 1)
    {
        std::cerr << "지원하지 않는 인수입니다. --help를 확인하세요.\n";
        return 2;
    }

    std::unique_ptr<dyf::RHI::IDevice> device(dyf::RHI::IDevice::Create(dyf::RHI::DeviceDesc{}));
    if(!device)
    {
        std::cerr << "RHI 장치를 생성하지 못했습니다. 사용 가능한 GPU, 선택한 백엔드와 드라이버 초기화를 확인하세요.\n";
        return 1;
    }

    PrintScope();
    std::cout << "다음 값은 현재 선택된 RHI 백엔드가 제공하는 기능/한도입니다.\n"
        "물리 GPU의 전체 기능 및 아직 없는 공개 실행 API의 사용 가능 여부와는 구분합니다.\n";
    PrintFeature(*device, "Rasterization", dyf::RHI::Feature::Rasterization);
    PrintFeature(*device, "Compute", dyf::RHI::Feature::Compute);
    std::cout << "  GetLimit(InlineConstantBytes) = "
        << device->GetLimit(dyf::RHI::Limit::InlineConstantBytes) << " 바이트\n";
    if(!device->Supports(dyf::RHI::Feature::Rasterization) && !device->Supports(dyf::RHI::Feature::Compute))
        std::cout << "현재 장치는 GPU raster/compute 실행을 제공하지 않습니다. Null 백엔드일 수 있으며, 장치 생성 성공만으로 GPU 가용성을 보장하지 않습니다.\n";

    // 완성 경로: 스레드별 독립 목록/불변 입력 분리 → 병렬 기록 계약 검증 → 제출 전 합류 → 직렬 Submit 비용 포함 측정.
    std::cout << "결과: 이 주제의 전체 실행 예제는 현재 제공하지 않습니다(종료 코드 77).\n";
    return 77;
}

