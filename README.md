# dy_engine

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat-square&logo=cmake)
![Compiler](https://img.shields.io/badge/Compiler-MSVC%20%7C%20GCC%20%7C%20Clang-555555?style=flat-square)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan%20%7C%20D3D12%20%7C%20Metal-4B5563?style=flat-square)

`dy_engine`은 현대 그래픽스 API의 설계 철학에 맞추어, 하드웨어 캐시 구조와 명시적 자원 모델에 맞춘 얇은 데이터 지향 렌더링 프레임워크입니다.

예제는 통합 실행기 `visualizer`의 GUI에서 클릭해 실행합니다. 실행 방법은 [아래 안내](#실행-방법)를 참고하세요.

## 목차

- [프로젝트 개요](#프로젝트-개요)
- [프로젝트 자료](#프로젝트-자료)
- [팀원 소개](#팀원소개)
- [주요 기능](#주요-기능)
- [설계 구조](#설계-구조)
- [요구 사항](#요구-사항)
- [CMake 옵션](#cmake-옵션)
- [빌드 방법](#빌드-방법)
- [실행 방법](#실행-방법)


## 프로젝트 개요

DirectX 12, Vulkan, Metal은 드라이버가 감추던 리소스 상태, 동기화, 파이프라인 구성을 애플리케이션이 직접 설계하도록 요구합니다. 이 방식은 런타임 유효성 검사와 암묵적 상태 추적 비용을 줄일 수 있지만, 두꺼운 범용 래퍼나 구형 상태 기계식 추상화를 얹으면 명시적 API의 장점이 상위 렌더링 단계까지 충분히 드러나지 않습니다.

`dy_engine`은 편의 기능을 하나의 큰 객체 계층에 모으기보다 데이터 흐름과 제어 책임에 따라 계층을 나눕니다. Foundation은 캐시 친화적 데이터 처리와 실행 기반을 맡고, RHI는 Vulkan, D3D12, Metal의 명시적 자원 모델을 얇게 노출합니다. 그 위에서 렌더링 경로는 패스, 리소스 수명, 바인딩 방식을 정리하고, Render Front-End는 사용자가 장면과 재질을 다루는 진입점이 됩니다.

사용자는 기본 렌더러로 장면을 구성할 수도 있고, 필요한 경우 `RendererDesc`, 상위 렌더러의 저수준 API, `IDevice`와 `ICommandList` 순으로 더 낮은 계층까지 내려가 제어할 수 있습니다. 목표는 모든 API를 포괄하는 범용성이 아니라, 최신 명시적 API의 자원 모델과 커맨드 기록 방식을 가리지 않는 점진적 제어 구조입니다.

## 프로젝트 자료

| 자료 | 링크 |
| --- | --- |
| 프로젝트 계획서 PPT | [docs/dy_engine_plan.pptx](docs/dy_engine_plan.pptx) |
| 프로젝트 발표 자료 | [docs/dy_engine_presentation.pdf](docs/dy_engine_presentation.pdf) |
| 프로젝트 보고서 | [docs/dy_engine_보고서_DyD팀.docx](docs/dy_engine_보고서_DyD팀.docx) |

## 팀원소개

| 이름 |  [![GitHub](https://img.shields.io/badge/한재승-181717?style=flat-square&logo=github)](https://github.com/suhanjin17)| [![GitHub](https://img.shields.io/badge/윤훈-181717?style=flat-square&logo=github)](https://github.com/yunhoon0206) | [![GitHub](https://img.shields.io/badge/정준혁-181717?style=flat-square&logo=github)](https://github.com/Wnsgur7318) |  [![GitHub](https://img.shields.io/badge/정현진-181717?style=flat-square&logo=github)](https://github.com/junghj0724)|
| :---: | :---: | :---: | :---: | :---: |
|역할 |Core/RHI|Vulkan|Metal|DirectX12|

## 주요 기능

- 얇은 RHI: `IDevice`, `ICommandList`, `IBuffer`, `ITexture`, `IPipelineState`로 상위 렌더러와 백엔드 구현을 분리합니다.
- 백엔드 선택: CMake 옵션으로 Vulkan, Direct3D 12, Metal, Null 백엔드를 선택합니다.
- 점진적 제어: 기본 렌더러 설정에서 시작해 필요하면 렌더러 저수준 API나 RHI 직접 사용으로 내려갈 수 있습니다.
- 렌더 패킷 구성: 장면, 메시, 재질, 조명 데이터를 정리하고 draw 제출 흐름을 예제 단위로 검증합니다.
- 리소스 바인딩 전략 비교: `07_RenderPath`에서 per-draw, batched, bindless binding mode를 실행 인자로 전환합니다.
- 명시적 그래픽스 API 실험: descriptor heap/table, push constant, pipeline state, command list 제출 흐름을 백엔드별로 연결합니다.
- 모델과 텍스처 로딩: OBJ, glTF, FBX 모델을 렌더링 경로에 올리고 재질/텍스처 연결을 검증합니다.
- 조명과 그림자: 대표 광원 데이터, shadow pass, main pass 연계를 통해 그림자 맵 기반 렌더링을 처리합니다.
- 데이터 지향 수학 경로: `DY_ENABLE_SIMD`로 SIMD 행렬 연산 경로를 켜고 끌 수 있습니다.

## 설계 구조

```text
Application / Examples
└─ 사용자 코드와 샘플 진입점

Layer 4. Render Front-End
└─ Renderer · Scene/Material · Flattened Render Queue

Layer 3. Render Path / Render Graph
└─ Pass 선언 · 리소스 수명/배리어 · transient 리소스 흐름

Layer 2. RHI
└─ IDevice · ICommandList · Resources · Backends(Vulkan/D3D12/Metal/Null)

Layer 1. Foundation
└─ Math(SIMD) · Platform · Core · Job/Task 기반

Cross-Cutting
└─ frame indexing · double/triple buffering · thread-local command recording
```

### RHI 계층

`RHI::IDevice`, `ICommandList`, `IBuffer`, `ITexture`, `IPipelineState`를 기준으로 상위 renderer가 특정 그래픽 API에 직접 의존하지 않도록 구성합니다.

### Graphics 계층

`Renderer`, `Scene`, `Mesh`, `RenderPath`를 중심으로 모델, 재질, 조명, 렌더링 경로를 관리합니다. 상위 계층은 RHI 인터페이스를 통해 리소스와 command list를 사용하고, API별 호출은 직접 다루지 않습니다.

선택 기능은 `src/Extends`에 둡니다. LTC 면광원의 구현과 표는
`src/Extends/AdvancedRendering`이 소유하며 코어에는 내장하거나 업로드하지 않습니다.
면광원을 쓰는 렌더러는 `ClusteredLighting` 모듈을 등록합니다. `AdvancedRendering` 프리셋에는
이 모듈이 포함되며, 공간 분류 옵션을 꺼도 LTC 조명은 유지됩니다.

### API 구현 계층

그래픽스 API 구현은 [CMake 옵션](#cmake-옵션)으로 선택합니다.

### Window와 진단 연결

공개 `Platform::Window`는 공통 입력과 이벤트 펌프를 제공하는 진입점입니다. 내부 `WindowBackend`를 Android·iOS 구현과 GLFW 공통 구현이 상속하고, GLFW 구현을 Windows·macOS·Linux가 공유합니다. CMake는 해당 플랫폼 구현 하나를 선택합니다.

데스크톱 창은 엔진이 생성하고 파괴합니다. Android의 `ANativeWindow`와 iOS의 `UIView`는 플랫폼이 소유하며, 이를 사용하는 Window와 RHI 장치보다 오래 살아 있어야 합니다. `GetNativeHandle()`은 주소와 종류를 함께 반환합니다. 플랫폼 앱의 연결은 내부 구현이 담당하며 공개 헤더로 노출하지 않습니다. iOS는 UIKit 메인 스레드에서 화면·입력을 관리하고, 한 실행 스레드가 공통 예제의 Window·입력·렌더링 루프를 순서대로 소유합니다.

프로파일러 공개 API는 기존 `dyf/Platform/Profiler.h`와 `dyf::Platform`에 둡니다. 공통 시계·CPU 구간·프레임/GPU 집계·스냅샷은 `src/Platform/Profiler`가 담당하며, HUD와 UI는 같은 측정 데이터를 사용합니다. Tracy와 RenderDoc 어댑터는 각각 `src/Extends/Tracy`, `src/Extends/RenderDoc`에 두고, 앱이 필요한 `dyf::Tracy`, `dyf::RenderDoc` 타깃을 링크합니다. 내부 분석용 통신 구현은 `src/Platform/Profiler/Connection`에 두며 HUD와 예제 실행기에서는 사용하지 않습니다. 공개 측정·집계는 `dyf/Platform/Profiler.h`의 `Profiler`로 제공하고 상세 수집·통신 계약은 비공개로 두며, 코어는 외부 SDK를 포함하거나 어댑터를 링크하지 않습니다. Tracy·RenderDoc 전용 라벨·문자열 복사·마커 전달은 코드의 `#ifdef`로도 제외합니다. 사용법은 [프로파일링과 캡처](docs/profiling.md)를 참고하세요.

Android 앱은 최상위 `android/`, iOS 앱과 Xcode 프로젝트는 최상위 `ios/`에 있습니다. Windows·Linux 실행 파일과 두 모바일 앱은 모두 `examples/main.cpp`의 선택 화면과 CMake 예제 목록을 사용합니다. 데스크톱은 선택한 실행 파일을 띄우고, 모바일은 같은 앱에 연결한 예제 진입 함수를 호출합니다. 예제가 끝나면 선택 화면으로 돌아옵니다.

## 요구 사항

먼저 빌드할 그래픽스 백엔드를 하나 고릅니다. 공통으로는 CMake와 C++17 컴파일러가 필요하고, 선택한 백엔드에 따라 플랫폼 SDK나 개발 패키지가 추가로 필요합니다.

- CMake 3.20 이상
- C++17 지원 컴파일러
- Git

### Dependencies

GLFW, stb, fastgltf, ufbx는 CMake `FetchContent`로 가져옵니다. 처음 configure할 때는 네트워크 연결이 필요할 수 있습니다.

### Backend Requirements

| 백엔드 | 대상 환경 | 추가 요구 사항 |
| --- | --- | --- |
| `DirectX 12` | Windows | MSVC C++ 빌드 도구, Windows SDK |
| `Vulkan` | Windows 또는 Linux | Vulkan SDK 또는 Vulkan 개발 패키지, `glslc` |
| `Metal` | macOS | Xcode 또는 Command Line Tools |
| 옵션 없음 | 모든 플랫폼 | Null 백엔드. 렌더링 API 없이 인터페이스 빌드 확인용 |

Ubuntu/Debian에서 Vulkan을 빌드할 때는 보통 다음 패키지가 필요합니다.

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config
sudo apt-get install -y libvulkan-dev vulkan-tools vulkan-validationlayers-dev glslc
```

GLFW 창 시스템은 `DY_LINUX_WINDOW_SYSTEM`으로 고를 수 있습니다. `AUTO`는 Wayland 세션에서 Wayland를 선택하며, 그 밖의 환경에서는 X11 개발 패키지를 찾은 뒤 없으면 Wayland를 선택합니다. Wayland에서는 ImGui를 포함해도 X11 개발 패키지가 필요하지 않습니다.

Wayland 개발 패키지:

```bash
sudo apt-get install -y libwayland-dev wayland-protocols libxkbcommon-dev
```

X11을 선택할 때는 대신 `xorg-dev`를 설치합니다.

## CMake 옵션

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `USE_VULKAN` | `OFF` | Windows에서 Vulkan 선택 |
| `USE_D3D12` | `OFF` | Direct3D 12 사용. Windows 전용 |
| `DY_ENABLE_SIMD` | `ON` | 지원 CPU에서 SIMD 수학 경로 사용 |
| `DY_LINUX_WINDOW_SYSTEM` | `AUTO` | Linux GLFW window system 선택. `AUTO`, `X11`, `WAYLAND` |
| `DY_ENABLE_TRACY` | `ON` | 선택적 Tracy CPU 구간·프레임·수치 연결 |
| `DY_ENABLE_LIVE_PROFILING` | `OFF` | 내부 Live 실행 진단 수집 |
| `DY_ENABLE_RENDERDOC` | `OFF` | 이미 주입된 RenderDoc API로 캡처 요청 및 D3D12 디버그 이벤트·마커 포함 |

Apple에서는 Metal, Linux·Android에서는 Vulkan을 자동 선택합니다. Windows에서만 `USE_D3D12` 또는 `USE_VULKAN` 중 하나를 켜며, 둘 다 끄면 Null을 사용합니다.

D3D12에서 `DY_ENABLE_RENDERDOC=ON`이면 WinPixEventRuntime 이벤트·마커 지원도 포함하며, 이 구성은 MSVC가 필요합니다. `OFF`이면 해당 런타임을 다운로드하거나 링크하지 않습니다.

## 빌드 방법

아래 예시는 generator를 지정하지 않습니다. CMake가 환경에 맞는 기본 generator를 고르게 두는 쪽이 보편적입니다. Visual Studio, Xcode, Ninja처럼 특정 generator가 필요하면 `-G` 옵션을, 특정 아키텍처를 지정하려면 `-A` 옵션을 사용하십시오.

빌드 디렉터리는 `build/` 하나를 사용합니다. 데스크톱 예제와 visualizer는 `build/examples/`, Showcase 실행 파일은 `build/showcase/`에 생성합니다. generator나 백엔드를 바꿀 때는 기존 `build/` 내용을 비우고 다시 구성합니다.

### Vulkan

```bash
cmake -S . -B build -DUSE_VULKAN=ON
cmake --build build --config Release
```

Linux에서 Wayland 전용 빌드는 다음과 같이 구성합니다. X11을 선택하려면 `WAYLAND`를 `X11`로 바꿉니다.

```bash
cmake -S . -B build -DDY_LINUX_WINDOW_SYSTEM=WAYLAND -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### DirectX 12

```powershell
cmake -S . -B build -DUSE_D3D12=ON
cmake --build build --config Release
```

### Metal

```bash
cmake -S . -B build -DUSE_METAL=ON
cmake --build build --config Release
```

### Null

```bash
cmake -S . -B build
cmake --build build --config Release
```

SIMD 비교가 필요하면 같은 백엔드 설정에 `-DDY_ENABLE_SIMD=OFF`를 추가해 다시 구성합니다.

```bash
cmake -S . -B build -DUSE_VULKAN=ON -DDY_ENABLE_SIMD=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

아래는 generator 지정 예시입니다.

```bash
cmake -S . -B build -DUSE_D3D12=ON -G "Visual Studio <your_version>" -A "<x64|Win32>"
cmake --build build --config Release
```

## 프레임워크 개발자 CI

자동 검사는 모든 브랜치의 GitHub `push` 이후 Actions에서 실행합니다. 로컬 pre-push로 전송을 막지 않으며, 실패한 작업 브랜치는 수정 후 다시 push합니다. main 병합 차단에는 관리자가 `CI Required`를 필수 상태 검사로 지정해야 합니다. 소비자 프로젝트에는 예제나 CI 검사를 자동 등록하지 않습니다.

push 전후의 변경 파일과 CMake 의존 관계로 필요한 예제·백엔드를 자동 선택합니다. 알려진 문서만 바뀌면 제품 검사를 생략하고, 공통 코드 변경이나 영향 범위가 불명확한 경우에는 전체 검사합니다.

공식 예제는 원본 코드 그대로 빌드·실행하고 외부에서 창 응답·화면·정상 종료를 확인합니다. 정밀한 CPU/GPU 기능 검사는 공개 API를 사용하는 별도 실행기에서 수행합니다. 예제 성공과 독립 API 검사 성공은 구분하여 보고합니다.

```sh
python -B .github/ci/ci.py run --phase cpu --api null
python -B .github/ci/ci.py run --phase runtime --api vulkan
```

지원 범위는 각 예제 CMake, 외부 관찰 조건은 `.github/ci/examples.json`에 선언합니다. 검증할 수 없는 경우를 성공으로 취급하지 않습니다. 현재 macOS 외부 관찰은 미구현으로 Metal runtime은 BLOCKED입니다.

[CI 사용법](.github/ci/README.md)

## 실행 방법

저장소 루트에서 예제와 통합 실행기를 빌드합니다. 아래 구성은 Model·ImGui·고급 렌더링을 포함합니다.

```bash
cmake -S . -B build -DDY_BUILD_EXAMPLES=ON -DDY_EXTEND_MODEL=ON -DDY_EXTEND_IMGUI=ON -DDY_EXTEND_ADVANCED_RENDERING=ON
cmake --build build --config Release --target dyf_examples
./build/examples/visualizer
```

Windows의 multi-config 빌드에서는 `build/examples/Release/visualizer.exe`를 실행합니다.
Windows에서는 구성 명령에 `-DUSE_D3D12=ON` 또는 `-DUSE_VULKAN=ON`을 추가해 그래픽 백엔드를 선택합니다.
실행하면 GUI 예제 목록이 열립니다. 왼쪽에서 dyf·RHI·advanced·showcase 분류를 고르고, 목록을 스크롤하며 예제를 클릭해 실행합니다.
예제 창을 닫으면 다른 예제를 실행할 수 있습니다. 실행기 창은 예제가 실행되는 동안에도 응답하며 종료 상태를 표시합니다.
실행기가 각 예제의 작업 폴더를 맞추므로 저장소 루트에서 바로 실행할 수 있습니다.
showcase의 에셋 경로는 `showcase/CMakeLists.txt`에서 설정합니다. 기본 에셋 루트는 저장소 옆 `../assets`이며,
`-DDY_ASSET_ROOT=/경로/assets`로 바꿀 수 있습니다. Cinematic은 Model·AdvancedRendering, Culling은 AdvancedRendering 확장이 필요합니다.
`DY_BUILD_SHOWCASE=ON`이면 누락된 기본 에셋도 구성 단계에서 자동으로 다운로드하며, 준비된 파일은 재사용합니다.
Android 프로젝트는 하나의 Examples APK를, iOS Xcode 프로젝트는 같은 선택 화면을 가진 앱을 만듭니다.
모바일 에셋도 예제별로 묶어 배포하며, Android Back과 iOS 돌아가기 버튼은 실행 중인 예제를 닫고 목록으로 돌아옵니다.
모바일에서 지원하지 않는 ImGui 편집기는 목록에 `N/A`로 표시됩니다. 각 예제의 GPU 기능 지원 여부는 별도로 적용됩니다.

```bash
./build/examples/visualizer --list
./build/examples/visualizer DyfScene
./build/examples/visualizer AdvancedProfiling --frames 120
```

이 실행기는 GUI 예제 선택 창입니다.
