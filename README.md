# dy_engine

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat-square&logo=cmake)
![Compiler](https://img.shields.io/badge/Compiler-MSVC%20%7C%20GCC%20%7C%20Clang-555555?style=flat-square)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan%20%7C%20D3D12%20%7C%20Metal-4B5563?style=flat-square)

`dy_engine`은 현대 그래픽스 API의 설계 철학에 맞추어, 하드웨어 캐시 구조와 명시적 자원 모델에 맞춘 얇은 데이터 지향 렌더링 프레임워크입니다.

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

### API 구현 계층

그래픽스 API 구현은 [CMake 옵션](#cmake-옵션)으로 선택합니다.

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

GLFW 창 시스템은 `DY_LINUX_WINDOW_SYSTEM`으로 고를 수 있습니다. X11을 사용할 때는 `xorg-dev`, Wayland를 사용할 때는 Wayland 개발 패키지가 필요합니다.

```bash
sudo apt-get install -y xorg-dev
sudo apt-get install -y libwayland-dev wayland-protocols libxkbcommon-dev
```

## CMake 옵션

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `USE_VULKAN` | `OFF` | Vulkan 사용 |
| `USE_D3D12` | `OFF` | Direct3D 12 사용. Windows 전용 |
| `USE_METAL` | `OFF` | Metal 사용. macOS 전용 |
| `DY_ENABLE_SIMD` | `ON` | 지원 CPU에서 SIMD 수학 경로 사용 |
| `DY_LINUX_WINDOW_SYSTEM` | `AUTO` | Linux GLFW window system 선택. `AUTO`, `X11`, `WAYLAND` |

그래픽스 백엔드 옵션은 한 번에 하나만 켜는 것을 기준으로 합니다. 백엔드를 바꿔 실험할 때는 빌드 폴더를 분리하면 CMake cache 충돌을 피할 수 있습니다.

## 빌드 방법

아래 예시는 generator를 지정하지 않습니다. CMake가 환경에 맞는 기본 generator를 고르게 두는 쪽이 보편적입니다. Visual Studio, Xcode, Ninja처럼 특정 generator가 필요하면 `-G` 옵션을, 특정 아키텍처를 지정하려면 `-A` 옵션을 사용하십시오.

### Vulkan

```bash
cmake -S . -B build/vulkan -DUSE_VULKAN=ON
cmake --build build/vulkan --config Release
```

### DirectX 12

```powershell
cmake -S . -B build/d3d12 -DUSE_D3D12=ON
cmake --build build/d3d12 --config Release
```

### Metal

```bash
cmake -S . -B build/metal -DUSE_METAL=ON
cmake --build build/metal --config Release
```

### Null

```bash
cmake -S . -B build/null
cmake --build build/null --config Release
```

SIMD 비교가 필요하면 같은 백엔드 설정에 `-DDY_ENABLE_SIMD=OFF`를 추가하고 별도 빌드 폴더를 사용합니다.

```bash
cmake -S . -B build/vulkan-nosimd -DUSE_VULKAN=ON -DDY_ENABLE_SIMD=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/vulkan-nosimd --config Release
```

아래는 generator 지정 예시입니다.

```bash
cmake -S . -B build/directx -DUSE_D3D12=ON -G "Visual Studio <your_version>" -A "<x64|Win32>"
cmake --build build/directx --config Release
```

## 실행 방법

예제는 빌드 후 생성된 실행 파일 위치에서 실행합니다. 셰이더와 모델 파일이 실행 파일 폴더로 복사되므로, 다른 작업 디렉터리에서 직접 실행하면 상대 경로가 맞지 않을 수 있습니다.

먼저 필요한 예제 타깃을 빌드합니다.

```bash
cmake --build build/vulkan --config Release --target Cube
```

실행 파일 위치는 사용하는 generator에 따라 조금 다릅니다.

| 빌드 방식 | 실행 위치 예시 |
| --- | --- |
| Windows multi-config generator | `build/vulkan/examples/03_Cube/Release/Cube.exe` |
| macOS multi-config generator | `build/metal/examples/03_Cube/Release/Cube` |
| Ninja, Makefile 같은 single-config generator | `build/vulkan/examples/03_Cube/Cube` |

대표 예제:

| 예제 | CMake 타깃 | 예제 폴더 |
| --- | --- |
| Hello Window | `HelloWindow` | `examples/01_HelloWindow` |
| Hello Renderer | `HelloRenderer` | `examples/02_HelloRenderer` |
| Cube | `Cube` | `examples/03_Cube` |
| Textured Cube | `TexturedCube` | `examples/04_TexturedCube` |
| Load Model | `LoadModel` | `examples/05_LoadModel` |
| Shadow Cube | `ShadowCube` | `examples/06_ShadowCube` |
| Render Path | `07_RenderPath` | `examples/07_RenderPath` |

`07_RenderPath`는 binding mode와 instance count를 실행 인자로 받을 수 있습니다.

```powershell
.\07_RenderPath.exe --per-draw --count=10000
.\07_RenderPath.exe --batched --count=10000
.\07_RenderPath.exe --bindless --count=10000
```

```bash
./07_RenderPath --per-draw --count=10000
./07_RenderPath --batched --count=10000
./07_RenderPath --bindless --count=10000
```
