<h1 align="center">dy_framework</h1>

<p align="center">
  <strong>쉽게 쓰고, 깊게 제어하는 렌더링 프레임워크</strong><br>
  데이터 지향 설계와 명시적 GPU 제어를 위한 C++ 프레임워크<br>
  <strong>동양미래대학교 컴퓨터소프트웨어공학과 · DyD 팀 작품</strong>
</p>

<p align="center">
  <a href="https://github.com/dyd-dev/dy_engine/actions/workflows/ci.yml"><img src="https://github.com/dyd-dev/dy_engine/actions/workflows/ci.yml/badge.svg?branch=main" alt="main 브랜치 빌드 CI"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus" alt="C++17">
  <img src="https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat-square&logo=cmake" alt="CMake 3.20 이상">
  <img src="https://img.shields.io/badge/Graphics-Vulkan%20%7C%20D3D12%20%7C%20Metal-556270?style=flat-square" alt="Vulkan, Direct3D 12, Metal">
</p>

<p align="center">
  <a href="https://dyd-dev.github.io/dy_engine/">API 문서</a> ·
  <a href="https://www.youtube.com/watch?v=STVjKfLLPLw">시연 영상</a> ·
  <a href="#빠른-시작">빠른 시작</a> ·
  <a href="#쇼케이스">쇼케이스</a> ·
  <a href="#구조">구조</a> ·
  <a href=".github/assets/dy-framework.pptx">발표자료</a> ·
  <a href="#문제-해결">문제 해결</a> ·
  <a href="#제작진">제작진</a>
</p>

[![dy_framework 시연 영상 보기 — Emerald Square 렌더링 화면](.github/assets/emerald-square.png)](https://www.youtube.com/watch?v=STVjKfLLPLw)

<p align="center"><a href="https://www.youtube.com/watch?v=STVjKfLLPLw"><strong>▶ YouTube에서 시연 영상 보기</strong></a></p>

<p align="center"><sub>Emerald Square · dy_framework 실제 렌더링 화면 · 원본 에셋 출처는 <a href="#라이선스와-크레딧">크레딧</a> 참고</sub></p>

## 소개

**dy_framework(`dyf`)는 동양미래대학교 컴퓨터소프트웨어공학과 DyD 팀이 제작한 렌더링 프레임워크입니다.** 저장소와 CMake 라이브러리 타깃의 이름은 `dy_engine`입니다.

Vulkan, Direct3D 12, Metal에서는 애플리케이션이 자원 상태와 동기화, GPU 명령 기록을 직접 관리합니다. dy_framework는 이 제어권을 공통 RHI(Rendering Hardware Interface)로 제공하고, 그 위에 장면·재질·조명·2D 그리기를 위한 API를 구성했습니다.

기본 렌더러로 화면을 만들고, 필요한 효과를 확장 모듈로 연결하며, 직접 GPU 작업을 구성할 때는 RHI를 사용할 수 있습니다. 장면 데이터와 GPU 자원 소유를 분리하고, 데이터 지향 처리와 SIMD, 병렬 명령 기록을 설계에 반영했습니다.

### 어디에서 시작하나요?

| 만들고 싶은 것 | 시작할 API와 예제 |
| --- | --- |
| 도형·이미지·텍스트가 있는 2D 화면 | `Canvas` + `Renderer` · [기본 도형 코드](examples/dyf/01_Primitives/main.cpp) · [전체 파일](examples/dyf/01_Primitives/) |
| 모델·재질·조명·애니메이션을 사용하는 3D 장면 | `Scene` + `Renderer`, 필요에 따라 Model·AdvancedRendering 확장 · [모델·애니메이션 코드](examples/dyf/07_ModelAnim/main.cpp) · [전체 파일](examples/dyf/07_ModelAnim/) |
| 직접 구성하는 렌더링·컴퓨트 패스와 GPU 명령 | `RHI::IDevice` + `ICommandList` · [장치와 화면 표시 코드](examples/RHI/01_DeviceAndPresent/main.cpp) · [전체 파일](examples/RHI/01_DeviceAndPresent/)<br>패스 구성으로 이어가기: [RenderGraph 코드](examples/RHI/11_RenderGraph/main.cpp) · [전체 파일](examples/RHI/11_RenderGraph/) |

앱의 실행 흐름과 장면 데이터는 사용자 코드에서 구성합니다. 기본 렌더러를 사용할 때는 `dyf` 예제부터, 자원 상태와 제출을 직접 제어할 때는 RHI 예제부터 읽으면 됩니다.

## 주요 기능

| 영역 | 제공 기능 |
| --- | --- |
| **공통 그래픽 API** | Vulkan·Direct3D 12·Metal 백엔드, 버퍼·텍스처·파이프라인·명령 목록·제출 제어 |
| **2D·3D 렌더링** | Canvas 도형·이미지·텍스트, Scene·Camera·Mesh·Material 기반 장면 구성 |
| **재질과 조명** | PBR 재질, 방향광·점광원·스폿 광원, 그림자, 환경광·반사 프로브, LTC 면광원 |
| **고급 렌더링** | SSAO·SSGI·SSR, TAA, Bloom, 볼륨 안개, 투명·굴절, HDR 후처리 |
| **대규모 장면 처리** | 인스턴싱·배칭, 메시 LOD, GPU 프러스텀·Hi-Z 컬링, indirect draw |
| **모델과 애니메이션** | glTF/GLB·FBX·OBJ 로딩, 골격 애니메이션과 GPU 스키닝 |
| **프레임 구성** | RenderGraph의 패스 의존성·자원 배리어 처리, 명령 병렬 기록 |
| **수학과 진단** | x86·ARM SIMD 수학 경로, 프로파일러 오버레이, Tracy·RenderDoc 연동 |

고급 효과와 모델 로딩, ImGui는 선택 확장으로 제공합니다. 기능별 요구 조건은 [모듈과 API 계층](https://dyd-dev.github.io/dy_engine/component/modules.html)을 참고하세요. GPU 기능은 장치의 지원 여부와 한도에 따라 달라집니다.

## 지원 플랫폼

| 플랫폼 | 그래픽 백엔드 | 창·앱 구성 | CI 빌드 구성 |
| --- | --- | --- | --- |
| Windows | Vulkan / Direct3D 12 | GLFW | 각 백엔드 Debug·Release |
| Linux | Vulkan | GLFW · X11 / Wayland | Debug·Release |
| macOS | Metal | GLFW · AppKit | Debug·Release |
| Android | Vulkan | 네이티브 앱 · Android NDK | Debug·Release, arm64-v8a·x86_64 |
| iOS | Metal | 네이티브 앱 · UIKit | Debug·Release, 기기·시뮬레이터 arm64, 서명 제외 |

백엔드는 **빌드 시 선택**합니다. Windows에서는 `USE_VULKAN` 또는 `USE_D3D12` 중 하나를 켜며, 둘 다 끄면 실제 화면을 렌더링하지 않는 Null 백엔드를 사용합니다. Linux·Android는 Vulkan, macOS·iOS는 Metal을 자동 선택합니다.

CI는 컴파일과 패키징을 확인합니다. 모든 기기의 실행 결과나 성능을 보장하는 검증은 아닙니다. 자세한 범위는 [CI 안내](.github/ci/README.md)에서 확인할 수 있습니다.

## 쇼케이스

![dy_framework로 렌더링한 Bistro 외부의 재질과 환경광](.github/assets/bistro.png)

*Bistro Exterior — 외부 모델의 재질과 환경 조명을 dy_framework의 렌더링 경로로 구성한 화면입니다.*

| 장면 | 시연 내용 |
| --- | --- |
| **Bistro Exterior / Night** | 외부 공간의 재질·환경광·그림자, 야간 광원과 후처리 |
| **Bistro Interior / Wine** | 실내 조명·반사 프로브, 유리의 투명·굴절·흡수 |
| **Emerald Square** | 건물과 식생으로 구성된 대규모 장면, LOD·컬링·와이어프레임 |
| **Arena** | 골격 애니메이션, 반복 객체의 인스턴싱과 배칭 |
| **Culling** | 공유 메시 장면에서 GPU 가시성 처리와 LOD 비교 |

`ShowcasePresentation`은 Bistro Exterior → Bistro Interior → Emerald Square를 같은 창에서 연속 시연합니다. 장면마다 기능을 켜고 끄며 결과를 비교할 수 있습니다.

- [프로젝트 시연 영상 · YouTube](https://www.youtube.com/watch?v=STVjKfLLPLw)
- [프로젝트 발표자료 · PPTX](.github/assets/dy-framework.pptx)
- [연속 시연과 조작 방법](showcase/cinematic/Presentation.md)
- [장면별 설명과 측정 조건](showcase/Presentation.md)

## 구조

```mermaid
flowchart TB
    App[Application / Examples / Showcase]
    API[dyf API · Scene / Canvas / Camera / Material]
    Ext[Extensions · Model / AdvancedRendering / ImGui]
    Renderer[Renderer · Prepare / RenderGraph / Pipelines]
    RHI[RHI · Device / CommandList / Resources / Submit / Present]
    Backends[Vulkan · Direct3D 12 · Metal]
    Platform[Platform · Window / Input / Profiler]
    Math[Math · SIMD]

    App --> API
    App --> Ext
    App -->|직접 GPU 제어| RHI
    API --> Renderer
    Ext --> Renderer
    Renderer --> RHI
    RHI --> Backends
    App -.-> Platform
    Renderer -.-> Math
```

`Scene`과 `Canvas`는 CPU 입력 데이터를 보관하고, `Renderer`는 렌더링에 필요한 GPU 자원을 관리합니다. RHI를 직접 사용하는 앱은 자원 생성부터 명령 기록·제출·표시까지 직접 구성합니다. `RenderGraph`는 선언된 패스의 의존성과 배리어를 계산합니다.

| 경로 | 역할 |
| --- | --- |
| [`src/Public/`](src/Public/) | 사용자에게 노출하는 `dyf`, RHI, Math, Platform, 확장 헤더 |
| [`src/dyf/`](src/dyf/) | 장면 처리, 렌더러, 조명·그림자·GPU 가시성 처리 |
| [`src/RHI/`](src/RHI/) | 공통 그래픽 인터페이스 구현, RenderGraph, 명령 처리 |
| [`src/Backends/`](src/Backends/) | Vulkan·D3D12·Metal·Null 구현 |
| [`src/Platform/`](src/Platform/) | 창, 입력, 시간, 프로파일러 |
| [`src/Extends/`](src/Extends/) | Model·AdvancedRendering·ImGui·Tracy·RenderDoc |
| [`examples/`](examples/) · [`showcase/`](showcase/) | API 학습용 예제와 발표용 장면 |
| [`android/`](android/) · [`ios/`](ios/) | 모바일 앱 프로젝트 |
| [API 문서 · GitHub Pages](https://dyd-dev.github.io/dy_engine/) · [`tests/`](tests/) | API 문서와 회귀·계약 테스트 |

## 빠른 시작

### 준비물

- Git, **CMake 3.20 이상**, C++17 지원 컴파일러. Showcase에는 C++20 지원이 필요합니다.
- 선택한 그래픽 백엔드를 지원하는 GPU·드라이버와 아래 개발 도구.
- 첫 CMake 구성 시 외부 의존성을 내려받을 수 있는 네트워크 환경.

| 환경 | 추가 개발 도구 |
| --- | --- |
| Windows / Vulkan | Visual Studio C++ 빌드 도구, Windows SDK, Vulkan SDK의 `glslc` |
| Windows / Direct3D 12 | Visual Studio C++ 빌드 도구, Windows SDK, `dxc` |
| Linux / Vulkan | GCC 또는 Clang, Vulkan 개발 라이브러리·`glslc`, X11 또는 Wayland 개발 패키지 |
| macOS / Metal | Xcode와 `xcrun`으로 실행할 수 있는 `metal`·`metallib` |
| Android / iOS | [Android 프로젝트](android/)의 SDK·NDK 설정 / [iOS 프로젝트](ios/)와 Xcode |

### 첫 예제 실행

```sh
git clone https://github.com/dyd-dev/dy_engine.git
cd dy_engine
```

**Windows · Vulkan · Visual Studio 2022**

```powershell
cmake -S . -B build/vulkan -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON -DDY_BUILD_EXAMPLES=ON -DDY_BUILD_SHOWCASE=OFF
cmake --build build/vulkan --config Release --target DyfPrimitives
.\build\vulkan\examples\dyf\01_Primitives\Release\DyfPrimitives.exe
```

Direct3D 12로 빌드하려면 별도 폴더 `build/d3d12`를 사용하고 `-DUSE_VULKAN=ON`을 `-DUSE_D3D12=ON`으로 바꿉니다. 설치한 Visual Studio 버전에 따라 `-G` 값도 맞춰 주세요.

**Linux · Vulkan / macOS · Metal · Ninja**

Ninja를 설치한 환경에서 실행합니다. 백엔드는 플랫폼에 맞게 자동 선택됩니다.

```sh
cmake -S . -B build/desktop -G Ninja -DCMAKE_BUILD_TYPE=Release -DDY_BUILD_EXAMPLES=ON -DDY_BUILD_SHOWCASE=OFF
cmake --build build/desktop --target DyfPrimitives
./build/desktop/examples/dyf/01_Primitives/DyfPrimitives
```

위 구성은 외부 장면 에셋이 필요 없는 기본 도형 예제로 시작합니다. `DY_BUILD_SHOWCASE=OFF`를 명시해 대용량 시연 에셋 준비를 제외했습니다. 모델·조명·ImGui 예제는 해당 확장을 추가로 켜야 합니다.

### 쇼케이스 실행

Windows Vulkan 구성 예시입니다. `DY_ASSET_ROOT`는 다운로드할 에셋의 위치이며, 기본 프리셋에서 필요한 원본 파일이 없으면 CMake 구성 중 다운로드·압축 해제를 수행합니다. 첫 구성에는 추가 시간과 저장 공간이 필요합니다.

```powershell
cmake -S . -B build/showcase-vulkan -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON -DDY_BUILD_SHOWCASE=ON -DDY_ASSET_ROOT="C:/dy-assets"
cmake --build build/showcase-vulkan --config Release --target ShowcaseCinematic ShowcasePresentation
.\build\showcase-vulkan\showcase\Release\showcase.exe emerald --profile desktop
.\build\showcase-vulkan\showcase\Release\presentation.exe
```

두 실행 명령은 각각 단일 장면과 연속 시연을 엽니다. `emerald` 대신 `exterior`, `night`, `interior`를 선택할 수 있습니다. 에셋별 이용 조건은 [크레딧](#라이선스와-크레딧)을 확인하세요.

### 내 프로젝트에 연결

소스를 `external/dy_engine`에 배치하고 앱의 `main.cpp`를 엔진 타깃과 연결합니다. 다음은 별도 앱 프로젝트의 `CMakeLists.txt`입니다. 기존 프로젝트에는 `add_subdirectory`와 링크 설정을 가져오면 됩니다.

```cmake
cmake_minimum_required(VERSION 3.20)
project(DyCanvasApp LANGUAGES CXX)

add_subdirectory(external/dy_engine)
add_executable(DyCanvasApp main.cpp)
target_link_libraries(DyCanvasApp PRIVATE dy_engine)
```

`main.cpp`는 아래 [Canvas 최소 앱](https://dyd-dev.github.io/dy_engine/component/examples/quickstart-canvas.cpp)으로 시작할 수 있습니다. 창을 먼저 만들고 렌더러를 나중에 생성해, 종료 시 GPU 자원이 창보다 먼저 정리되도록 합니다.

<details>
<summary><strong>Canvas 최소 앱 코드 · C++17</strong></summary>

```cpp
#include <dyf.h>
#include <cstdio>
#include <exception>

int main()
{
    try
    {
        constexpr uint32_t width = 640, height = 480;
        dyf::Platform::Window window(width, height, "dy_engine / Canvas");
        if (!window.GetHandle()) return 1;

        // 창은 렌더러보다 오래 살아 있어야 한다.
        auto renderer = dyf::Renderer::Create(window.GetHandle());
        if (!renderer) return 1;

        dyf::Canvas canvas(width, height);
        if (!canvas.IsValid()) return 1;
        canvas.Clear({0.04f, 0.06f, 0.10f, 1.0f});
        canvas.FillRect({80, 100, 220, 140}, {0.10f, 0.65f, 0.95f, 1.0f});
        canvas.FillCircle({430, 230}, 70.0f, {1.0f, 0.65f, 0.15f, 1.0f});
        canvas.Line({80, 340}, {540, 340}, {1, 1, 1, 1}, 3.0f);

        while (window.IsRunning())
        {
            window.PollEvents();
            if (!window.IsRunning()) break;
            // 같은 CPU 명령 목록을 여러 프레임에서 다시 그릴 수 있다.
            if (!renderer->Render(canvas)) return 1;
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
```

</details>

앱 폴더에서 다음과 같이 구성하고 빌드합니다. Windows는 앞서 준비한 Vulkan SDK와 C++ 도구를 사용합니다. Linux·macOS에서는 `-DUSE_VULKAN=ON`을 생략하면 플랫폼에 맞는 백엔드가 선택됩니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUSE_VULKAN=ON
cmake --build build --config Release --target DyCanvasApp
```

기본 API는 `<dyf.h>`, 저수준 API는 `<dyf/RHI.h>`로 시작합니다. 단계별 설명은 [빠른 시작](https://dyd-dev.github.io/dy_engine/component/quickstart.html), 자원 수명과 명령 제출은 [RHI 가이드](https://dyd-dev.github.io/dy_engine/component/rhi-lifecycle.html)를 참고하세요.

### 확장 연결

확장을 사용하려면 **CMake 구성 옵션을 켜고, 앱에 해당 타깃을 링크한 뒤, 확장 헤더를 포함**합니다. 옵션만 켜는 것으로 앱에서 모든 효과가 자동 적용되지는 않습니다.

| 기능 | 구성 옵션 | 앱에 링크할 타깃 |
| --- | --- | --- |
| 모델 로딩·애니메이션 | `DY_EXTEND_MODEL=ON` | `dyf::Model` |
| 고급 조명·투명·후처리 | `DY_EXTEND_ADVANCED_RENDERING=ON` | `dyf::AdvancedRendering` |
| Dear ImGui | `DY_EXTEND_IMGUI=ON` | `dyf::ImGui` |
| Tracy 프로파일링 | `DY_ENABLE_TRACY=ON` | `dyf::Tracy` |
| RenderDoc 캡처 | `DY_ENABLE_RENDERDOC=ON` | `dyf::RenderDoc` |

예를 들어 고급 렌더링은 `dyf::AdvancedRendering`을 링크하고 `<dyf/Extends/AdvancedRendering.h>`를 포함한 뒤, `Renderer::AddExtension`으로 프리셋 또는 필요한 모듈을 등록합니다. 헤더·초기화·설정 방법은 [모듈과 API 계층](https://dyd-dev.github.io/dy_engine/component/modules.html)을 참고하세요.

### 셰이더 준비

기본 렌더러의 셰이더는 CMake 빌드 중 컴파일되어 라이브러리에 내장됩니다. 사용자 셰이더는 선택한 백엔드에 맞는 소스와 바이너리를 준비합니다.

| 백엔드 | 셰이더 소스 | 컴파일 도구와 결과 |
| --- | --- | --- |
| Vulkan | GLSL · `.glsl` | `glslc` → SPIR-V |
| Direct3D 12 | HLSL · `.hlsl` | `dxc` → DXIL |
| Metal | Metal Shading Language · `.metal` | `xcrun metal` + `metallib` → Metal 라이브러리 |

`dy_compile_shader` 빌드 헬퍼는 [CompileShaders.cmake](cmake/CompileShaders.cmake), 바인딩·정점 입력 규약은 [Renderer 셰이더와 외부 입력 준비](https://dyd-dev.github.io/dy_engine/component/renderer-inputs.html)에서 확인할 수 있습니다. 런타임 GLSL 컴파일은 Vulkan에서 `DY_ENABLE_SHADER_COMPILER=ON`으로 빌드했을 때 제공되는 별도 기능입니다.

## CMake 옵션

아래는 엔진 루트를 새 빌드 폴더에 구성할 때의 기본값입니다. 기존 빌드 폴더는 캐시 값을 유지하므로 백엔드를 바꿀 때는 폴더를 분리합니다.

<details>
<summary><strong>백엔드·확장·진단 옵션 펼치기</strong></summary>

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `USE_VULKAN` | Windows: `OFF` | Windows에서 Vulkan 선택. Linux·Android에서는 자동 활성화 |
| `USE_D3D12` | Windows: `OFF` | Windows에서 Direct3D 12 선택. Vulkan과 동시 선택 불가 |
| `USE_METAL` | Apple: `ON` | macOS·iOS에서 자동 선택 |
| `DY_BUILD_EXAMPLES` | `OFF` | 최상위 빌드에서 단독 예제 타깃 추가 |
| `DY_BUILD_SHOWCASE` | `DY_BUILD_EXAMPLES` 값 | 발표용 장면과 에셋 준비, Model·AdvancedRendering 자동 활성화 |
| `DY_EXTEND_MODEL` | `OFF` | 모델 로딩·애니메이션 확장 |
| `DY_EXTEND_ADVANCED_RENDERING` | `OFF` | 고급 조명·안개·투명·후처리 확장 |
| `DY_EXTEND_IMGUI` | `OFF` | 데스크톱 Dear ImGui 확장 |
| `DY_ENABLE_SIMD` | `ON` | 지원 CPU에서 SIMD 수학 경로 사용 |
| `DY_ENABLE_TRACY` | `ON` | Tracy 계측과 연결 어댑터 |
| `DY_ENABLE_LIVE_PROFILING` | `OFF` | 실행 진단 수집 |
| `DY_ENABLE_RENDERDOC` | `OFF` | RenderDoc 캡처 연동. Metal에서는 사용 불가 |
| `DY_BUILD_CONFORMANCE_TESTS` | `OFF` | 회귀·계약 검사 타깃과 CTest 등록 |
| `DY_ENABLE_SHADER_COMPILER` | `OFF` | Vulkan용 선택적 GLSL 컴파일러 내장. CMake 3.27 이상 필요 |
| `DY_LINUX_WINDOW_SYSTEM` | `AUTO` | Linux 창 시스템: `AUTO`·`X11`·`WAYLAND` |

세부 설정은 [Engine_Options.cmake](cmake/Engine_Options.cmake), 셰이더 컴파일러 옵션은 [CMakeLists.txt](CMakeLists.txt)를 기준으로 합니다.

</details>

## 문서와 예제

| 목적 | 자료 |
| --- | --- |
| API 문서 홈 | [dy_framework API 문서](https://dyd-dev.github.io/dy_engine/) |
| 개발 환경 준비 | [시작하기](https://dyd-dev.github.io/dy_engine/component/getting-started.html) |
| 첫 화면 만들기 | [Canvas 앱 빠른 시작](https://dyd-dev.github.io/dy_engine/component/quickstart.html) |
| API 찾기 | [API 레퍼런스](https://dyd-dev.github.io/dy_engine/component/api-index.html) |
| 계층·확장 이해하기 | [모듈과 API 계층](https://dyd-dev.github.io/dy_engine/component/modules.html) |
| GPU 직접 제어 | [RHI 프레임과 자원 수명](https://dyd-dev.github.io/dy_engine/component/rhi-lifecycle.html) |
| 단계별 실습 | [dyf 예제](examples/dyf/) · [RHI 예제](examples/RHI/) · [고급 예제](examples/advanced/) |
| 프로젝트 소개 | [발표자료](.github/assets/dy-framework.pptx) · [쇼케이스 안내](showcase/Presentation.md) |
| 문제 제보·개선 제안 | [GitHub Issues](https://github.com/dyd-dev/dy_engine/issues) |

## 문제 해결

<details>
<summary><strong>빌드·실행 중 자주 확인할 항목</strong></summary>

| 증상 | 확인할 항목 |
| --- | --- |
| Windows에서 빌드는 되지만 화면이 렌더링되지 않음 | `USE_VULKAN`과 `USE_D3D12`가 모두 꺼져 있으면 Null 백엔드입니다. 둘 중 하나를 켜고 새 빌드 폴더에 구성합니다. |
| `glslc`·`dxc`를 찾지 못함 | 해당 컴파일러가 설치되어 있고 CMake에서 찾을 수 있는지 확인합니다. 필요하면 `-DDY_GLSLC="전체 경로"` 또는 `-DDY_DXC="전체 경로"`를 지정합니다. |
| Linux에서 창 시스템 의존성을 찾지 못함 | 선택한 X11 또는 Wayland의 개발 패키지를 준비합니다. `DY_LINUX_WINDOW_SYSTEM=X11` 또는 `WAYLAND`로 사용할 창 시스템을 명시할 수 있습니다. |
| 기본 예제만 빌드하려는데 대용량 에셋을 내려받음 | 새 구성에서는 `DY_BUILD_SHOWCASE`가 `DY_BUILD_EXAMPLES` 값을 기본값으로 사용합니다. `-DDY_BUILD_SHOWCASE=OFF`를 명시합니다. |
| 모델·조명·ImGui 예제 타깃이 없거나 확장 링크에 실패함 | 필요한 `DY_EXTEND_*` 옵션, 생성된 `dyf::*` 타깃, 앱의 `target_link_libraries` 설정을 함께 확인합니다. |
| 고급 효과를 켰는데 변화가 없음 | 해당 확장의 링크·모듈 등록과 Renderer 설정을 확인합니다. 고급 렌더링은 HDR·표준 지오메트리 등 각 모듈의 요구 조건도 충족해야 합니다. |
| 백엔드를 바꾼 뒤 설정이 충돌함 | 기존 `CMakeCache.txt`의 설정이 남아 있을 수 있습니다. Vulkan·D3D12 등 구성마다 빌드 폴더를 분리합니다. |

플랫폼별 준비 과정과 에셋 경로는 [시작하기](https://dyd-dev.github.io/dy_engine/component/getting-started.html), 현재 옵션은 [CMake 옵션](#cmake-옵션)을 참고하세요.

</details>

## 참여와 문의

버그 제보와 개선 제안은 [GitHub Issues](https://github.com/dyd-dev/dy_engine/issues), 코드·문서 수정은 [Pull requests](https://github.com/dyd-dev/dy_engine/pulls)를 이용해 주세요.

문제를 재현할 수 있도록 다음 정보를 함께 남겨 주세요.

- 사용한 커밋, OS, GPU·드라이버, 그래픽 백엔드
- CMake 구성·빌드·실행 명령과 최소 재현 예제
- 기대한 결과와 실제 결과, 오류 로그 또는 화면

API나 동작을 바꾸는 수정은 관련 예제·문서도 함께 갱신하고, 확인한 플랫폼·빌드 구성과 검증 결과를 적어 주세요. 프레임 시간이나 성능 문제는 해상도·VSync·장면·품질 설정을 함께 기록하면 비교에 도움이 됩니다.

## 제작진

**동양미래대학교 컴퓨터소프트웨어공학과**<br>
**DyD — Direct, Yield, Dynamic media**

| 제작자 | 담당 | GitHub · 공개 연락처 |
| --- | --- | --- |
| **한재승** | Core/RHI | [suhanjin17](https://github.com/suhanjin17) |
| **윤훈** | Vulkan | [yunhoon0206](https://github.com/yunhoon0206) |
| **정준혁** | Metal | [Wnsgur7318](https://github.com/Wnsgur7318) |
| **정현진** | DirectX12 | [junghj0724](https://github.com/junghj0724) |

## 라이선스와 크레딧

현재 저장소 루트에는 프레임워크 자체의 라이선스 파일이 포함되어 있지 않습니다. 외부 라이브러리와 시연 에셋에는 각각의 라이선스가 적용됩니다.

GLFW, stb, bcdec, meshoptimizer, fastgltf, ufbx, Dear ImGui, Tracy 등 외부 프로젝트를 사용합니다. 의존성 버전과 원본은 [의존성 설정](cmake/Dependencies.cmake) 및 각 [확장 모듈](src/Extends/)의 CMake 설정에서 확인할 수 있습니다.

README 이미지는 발표 영상 제작에 사용한 실제 렌더링 캡처를 재사용했습니다. 원본 장면을 프레임워크에서 불러와 좌표계·크기를 맞추고 자체 재질·조명·후처리 경로로 렌더링한 결과입니다.

| 이미지·에셋 | 원저작자와 출처 | 원본 이용 조건 |
| --- | --- | --- |
| `emerald-square.png` · NVIDIA Emerald Square | Nicholas Hull, Kate Anderson, Nir Benty · [NVIDIA ORCA](https://developer.nvidia.com/orca/nvidia-emerald-square), 식생: SpeedTree | CC BY-NC-SA 3.0 |
| `bistro.png` · Amazon Lumberyard Bistro | Amazon Lumberyard · [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) | CC BY 4.0 |

추가 에셋과 변환 내역은 [Cinematic 크레딧](showcase/cinematic/ASSET_CREDITS.md), [Arena 크레딧](showcase/arena/ASSET_CREDITS.md), [LTC 표 라이선스](src/Extends/AdvancedRendering/Assets/ltc/LICENSE)를 참고하세요.
