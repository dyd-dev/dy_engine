# Examples

`dyf/`는 Canvas·Scene·Camera 등 dyf의 사용법을, `RHI/`는 공개
`RHI::IDevice`와 `RHI::ICommandList`로 GPU 작업을 구성하는 방법을 보여줍니다.
RHI 기본 예제는 아래 순서대로 읽으면 됩니다. 각 폴더의 `description.txt`에도 주제를 적었습니다.

## RHI 기본 학습 순서

| 예제 | 다루는 API와 동작 | 빌드 타깃 |
| --- | --- | --- |
| [01_DeviceAndPresent](RHI/01_DeviceAndPresent/main.cpp) | Device·Swapchain 생성, Clear, 상태 전환, Close·Submit·Present, 해제 | `RhiDeviceAndPresent` |
| [02_GraphicsPipeline](RHI/02_GraphicsPipeline/main.cpp) | VS·FS와 topology·raster·출력 포맷으로 GraphicsPipeline 구성 | `RhiGraphicsPipeline` |
| [03_BuffersAndDrawCalls](RHI/03_BuffersAndDrawCalls/main.cpp) | Vertex·Index·Instance 버퍼 업로드, Draw와 Indexed·Instanced 비교 | `RhiBuffersAndDrawCalls` |
| [04_ConstantsAndBindings](RHI/04_ConstantsAndBindings/main.cpp) | PipelineLayout, Inline Constants, Uniform Buffer 정렬, ResourceSet | `RhiConstantsAndBindings` |
| [05_TexturesAndSamplers](RHI/05_TexturesAndSamplers/main.cpp) | Texture 업로드, rowPitch, Nearest·Linear 정적 Sampler 비교 | `RhiTexturesAndSamplers` |
| [06_DepthAndBlend](RHI/06_DepthAndBlend/main.cpp) | Depth attachment와 검사·쓰기, Alpha Blend 상태 구성 | `RhiDepthAndBlend` |
| [07_RenderTargetsAndBarriers](RHI/07_RenderTargetsAndBarriers/main.cpp) | 오프스크린 렌더링 결과를 샘플링하는 두 패스와 수동 Barrier | `RhiRenderTargetsAndBarriers` |
| [08_ComputePipeline](RHI/08_ComputePipeline/main.cpp) | CS·그룹 크기·Dispatch, Storage 쓰기 → Vertex 읽기 | `RhiComputePipeline` |
| [09_SubmissionAndFrames](RHI/09_SubmissionAndFrames/main.cpp) | 제출 완료 Fence, 프레임 슬롯, 완료 후 CommandList 재사용 | `RhiSubmissionAndFrames` |
| [10_ParallelRecording](RHI/10_ParallelRecording/main.cpp) | CPU 스레드별 CommandList로 독립된 Draw 기록, 합류·GPU Barrier·Fence 구분 | `RhiParallelRecording` |
| [11_RenderGraph](RHI/11_RenderGraph/main.cpp) | Compute → 오프스크린 Draw → 화면 합성을 Read·Write로 연결 | `RhiRenderGraph` |

01–08에서 장치·자원·Pipeline·패스와 명령을 구성하고, 09에서 제출·프레임 수명을 다룹니다.
10은 같은 프레임의 독립된 작업을 CPU에서 병렬 기록합니다. 11은 패스의 자원 접근 관계를
선언해 실행 의존성과 Barrier를 구성합니다.

01은 셰이더 없이 화면을 지우는 예제입니다. 02부터 각 예제의 `Shaders/`에
GLSL·HLSL·Metal 소스를 포함합니다. main과 셰이더를 함께 읽으면 입력 속성, 바인딩 번호,
상수 데이터와 셰이더 단계가 어떻게 연결되는지 확인할 수 있습니다.

각 RHI main은 창·이벤트 처리를 위한 `Platform::Window`와 공개 RHI를 직접 사용합니다.
장치 생성부터 리소스 해제까지 해당 main에 있으며, 다른 예제의 실행 코드나 공통 렌더러에
작업을 위임하지 않습니다. 화면을 닫을 때까지 실행하고, 크기는 현재 Backbuffer에서 읽습니다.

## Pipeline과 RenderGraph 읽는 법

02의 `GraphicsPipelineDesc`는 셰이더 단계와 정점 입력, topology, raster, 출력 상태를
묶습니다. `BindGraphicsPipeline`으로 선택한 뒤 `DrawInstanced`를 기록합니다.
`BeginRendering`은 이번 패스에서 사용할 첨부와 Load·Store·Clear를 지정합니다.

03에서 Indexed 여부는 인덱스 버퍼를 통한 정점 참조 여부이고, Instanced 여부는 같은
정점 구성을 몇 번 실행하는지입니다. 두 Draw 모두 바인딩한 GraphicsPipeline을 사용합니다.
04·05는 파이프라인의 바인딩 선언과 실제 Buffer·Texture 연결을 이어서 보여줍니다.

08의 `Dispatch`는 바인딩한 ComputePipeline의 CS를 실행하며 인자는 **스레드 그룹 수**입니다.
그룹당 스레드 수는 셰이더 선언과 `ComputePipelineDesc::threadGroupSize`를 맞춥니다.

07은 패스 사이 Barrier를 직접 기록합니다. 11은 각 패스의 Read·Write와 자원 경계 상태를
선언해 RenderGraph가 그 Barrier를 기록하게 합니다. `RenderGraphPass::SetPipeline`으로 패스의
Pipeline을 선택합니다. GPU Pipeline 생성과 각 패스의 Draw·Dispatch 본문도 main에 남습니다.
`RenderGraph::Execute`는 명령 기록이며, 이후 Close·Submit·Present는
호출자가 수행합니다.

Buffer·Texture의 Usage는 허용할 용도이고, ResourceState는 해당 시점의 접근 상태입니다.
CPU 데이터는 `UpdateBuffer`·`UpdateTexture` 호출 중 복사되며, GPU 업로드는 기록한 목록을
Submit해야 실행됩니다. 09는 두 프레임 슬롯에 완료 Fence와 명령 목록을 보관하고,
같은 슬롯으로 돌아왔을 때 완료를 기다려 목록을 Reset합니다. Present는 프레임 표시를 요청합니다.

## CPU 병렬 기록과 GPU 동기화

10은 두 작업 스레드가 각각 자신의 CommandList와 512×512 출력 텍스처로 삼각형을 그립니다.
메인 스레드는 작업 합류 후 별도의 CommandList에 두 결과를 화면 좌우에 표시하는 패스를 기록합니다.
모든 목록을 닫은 뒤 `PrepareCommandLists(..., 2)`로 네이티브 명령 변환도 병렬 기록하고,
작업 0 → 작업 1 → 화면 합성 순서로 한 번에 Submit합니다.

| 동기화 지점 | 목적 | 10의 코드 |
| --- | --- | --- |
| CPU 작업 합류 | 모든 작업 스레드의 명령 기록과 오류 수집 완료 | `std::jthread` 범위 종료 후 오류 확인 |
| GPU ResourceBarrier | 출력 쓰기와 후속 샘플링 사이의 상태 전환·메모리 의존성 | `RenderTarget → ShaderResource` |
| 제출 완료 Fence | 같은 프레임 슬롯의 목록·텍스처를 재사용할 시점 확인 | `Wait → ResetCommandList` |

각 스레드는 별도 목록을 사용하며, 공유 Pipeline과 자원 수명·프레임·Swapchain은 기록 중에
변경하지 않습니다. 작업 스레드의 종료 순서에 관계없이 GPU 목록의 제출 순서를 지정합니다.
이 예제의 병렬화 대상은 CPU 명령 기록입니다. GPU 실행에는 현재 RHI가 제공하는 장치 큐를 사용합니다.

## 기능 지원

모든 화면 예제에는 Rasterization을 사용합니다. 08·11에는 Compute도 필요합니다.
필수 기능이 없으면 해당 예제는 진단 메시지와 종료 코드 77을 반환합니다.
실제 기능 지원은 선택한 장치와 백엔드에 따릅니다.

## 빌드

전체 예제의 Vulkan 빌드:

```sh
cmake -S . -B build-vulkan -DUSE_VULKAN=ON -DDY_BUILD_EXAMPLES=ON
cmake --build build-vulkan
```

예제 하나만 빌드할 수도 있습니다.

```sh
cmake -S examples/RHI/02_GraphicsPipeline -B build-rhi-pipeline -DUSE_VULKAN=ON
cmake --build build-rhi-pipeline --target RhiGraphicsPipeline
```

폴더를 저장소 밖으로 복사했다면 `-DDY_ENGINE_SOURCE_DIR=/path/to/dy_engine`을 지정합니다.
Windows는 `-DUSE_D3D12=ON`, macOS는 `-DUSE_METAL=ON`을 사용합니다.
네이티브 백엔드는 하나만 선택합니다. Vulkan은 glslc, D3D12는 dxc,
Metal은 Xcode Metal 도구로 해당 폴더의 셰이더를 컴파일합니다.
`cmake/CompileShaders.cmake`가 바이트와 entry point를 담은 헤더를 생성하므로
main에는 백엔드별 분기가 필요하지 않습니다. Null은 이 화면 예제를 그리지 않습니다.

## dyf·advanced·legacy

`dyf/`는 도형·이미지·뷰포트·카메라·재질·조명·모델·장면·텍스트·ImGui 사용 예제입니다.
모델과 ImGui는 각 확장 빌드 옵션을 사용합니다. Text의 TrueType 폰트는
`DY_EXAMPLE_FONT`로 지정합니다. 이미지·모델 자산과 출처·라이선스는 해당 폴더에 있습니다.

`advanced/`는 프로파일링, 그림자, 간접 그리기, 병렬 기록 등 특정 주제를 다룹니다.
각 폴더의 설명에서 구현 범위와 필요한 기능을 확인하면 됩니다.

기존 RHI 장면 예제 10개와 자산은 [legacy/RHI](legacy/RHI)에 보존했습니다.
전체 예제 빌드에는 포함하지 않으며, 필요하면 해당 폴더를 독립적으로 빌드할 수 있습니다.
새 RHI 예제는 자체 셰이더와 생성한 정점·화소 데이터를 사용하므로 모델·폰트·이미지 파일을
별도로 준비할 필요가 없습니다.
