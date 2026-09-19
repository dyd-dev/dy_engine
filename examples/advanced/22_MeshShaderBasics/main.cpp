#include <dyf/RHI/IDevice.h>

#include <cstring>
#include <iostream>
#include <memory>

namespace
{
    void PrintScope()
    {
        std::cout <<
        "메시 셰이더 기초: 현재는 지원 조회와 미지원 단계 안내를 제공하는 예제입니다.\n"
        "Feature::MeshShader/TaskShader 조회 항목은 있으나 공개 ShaderStage는 Vertex/Fragment/Compute만 제공합니다.\n"
        "GraphicsPipelineDesc에 mesh/task shader를 지정하는 필드가 없고 ICommandList에 mesh 작업 실행 명령이 없습니다.\n"
        "일반 vertex shader 드로우를 mesh shader 실행으로 대체 표시하지 않으며 이 예제는 지원 진단만 수행합니다.\n"
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
        std::cout << "사용법: AdvancedMeshShaderBasics [--help]\n"
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
    PrintFeature(*device, "MeshShader", dyf::RHI::Feature::MeshShader);
    PrintFeature(*device, "TaskShader", dyf::RHI::Feature::TaskShader);

    if(!device->Supports(dyf::RHI::Feature::Rasterization) && !device->Supports(dyf::RHI::Feature::Compute))
        std::cout << "현재 장치는 GPU raster/compute 실행을 제공하지 않습니다. Null 백엔드일 수 있으며, 장치 생성 성공만으로 GPU 가용성을 보장하지 않습니다.\n";

    // 완성 경로: mesh/task shader stage와 pipeline 정의 → mesh 작업 명령/한도 노출 → 작은 메시 출력 → 결과 검증.
    std::cout << "결과: 이 주제의 전체 실행 예제는 현재 제공하지 않습니다(종료 코드 77).\n";
    return 77;
}

