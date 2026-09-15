#include <dyf/RHI/IDevice.h>

#include <cstring>
#include <iostream>
#include <memory>

namespace
{
    void PrintScope()
    {
        std::cout <<
        "하이브리드 렌더링: 현재는 지원 조회와 미지원 단계 안내를 제공하는 예제입니다.\n"
        "raster 장면과 지원 백엔드의 compute 후처리는 현재 RHI로 구성할 수 있습니다.\n"
        "ray 단계에는 BLAS/TLAS 생성·빌드·바인딩과 ray query 또는 RT shader/pipeline 실행 경로가 필요하지만 현재 공개 API에 없습니다.\n"
        "실제 ray 그림자/반사와 raster 결과를 합성하는 전체 경로는 실행하지 않습니다. 합성 입력을 가짜 ray 결과로 채우지 않습니다.\n"
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
        std::cout << "사용법: AdvancedHybridRendering [--help]\n"
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
    PrintFeature(*device, "RayQuery", dyf::RHI::Feature::RayQuery);
    PrintFeature(*device, "RayTracingPipeline", dyf::RHI::Feature::RayTracingPipeline);
    std::cout << "  GetLimit(Texture2DDimension) = "
        << device->GetLimit(dyf::RHI::Limit::Texture2DDimension) << " 픽셀\n";
    if(!device->Supports(dyf::RHI::Feature::Rasterization) && !device->Supports(dyf::RHI::Feature::Compute))
        std::cout << "현재 장치는 GPU raster/compute 실행을 제공하지 않습니다. Null 백엔드일 수 있으며, 장치 생성 성공만으로 GPU 가용성을 보장하지 않습니다.\n";

    // 완성 경로: raster G-buffer → scene 가속 구조 → ray 그림자/반사 → 필요시 디노이징 → 조명/합성 → 결과 검증.
    std::cout << "결과: 이 주제의 전체 실행 예제는 현재 제공하지 않습니다(종료 코드 77).\n";
    return 77;
}

