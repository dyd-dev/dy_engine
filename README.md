# dy_engine

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat-square&logo=cmake)
![Compiler](https://img.shields.io/badge/Compiler-MSVC%20%7C%20GCC%20%7C%20Clang-555555?style=flat-square)
![Graphics](https://img.shields.io/badge/Graphics-Vulkan%20%7C%20D3D12%20%7C%20Metal-4B5563?style=flat-square)

`dy_engine`은 Vulkan, Direct3D 12, Metal 같은 명시적 그래픽스 API를 공통 RHI(Render Hardware Interface) 위에서 실험하는 C++17 렌더링 프레임워크입니다. 두꺼운 범용 래퍼보다 API가 요구하는 리소스 관리, command 기록, 렌더링 데이터 흐름을 직접 드러내는 구조를 목표로 합니다.

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

현대 그래픽스 API는 드라이버가 암묵적으로 처리하던 상태 전이와 리소스 관리를 프로그래머에게 넘깁니다. Vulkan과 Direct3D 12는 Pipeline State Object, descriptor heap, command list, 명시적 동기화처럼 더 직접적인 제어 모델을 제공하지만, 범용 래퍼가 너무 두꺼우면 이런 장점이 다시 가려질 수 있습니다.

`dy_engine`은 이 지점에서 출발합니다. 상위 렌더러가 특정 API에 묶이지 않도록 RHI 계약을 두고, 하위 구현에서는 각 API의 장치, 리소스, command 제출 흐름을 직접 연결합니다. 또한 scene, mesh, material, render path를 분리하여 예제 단위로 렌더링 기능을 검증할 수 있게 구성했습니다.

관련 작품과 비교하면, bgfx처럼 폭넓은 호환성을 위한 최소 공통 분모를 목표로 하기보다 최신 명시적 API의 제어 모델을 학습하고 노출하는 데 집중합니다. MethaneKit처럼 최신 API를 대상으로 삼되, 본 프로젝트는 수업 프로젝트 범위 안에서 RHI 경계, 데이터 흐름, API 구현 분리를 직접 설계하고 검증하는 데 초점을 둡니다.

## 주요 기능

- RHI 기반 그래픽스 API 분리: `IDevice`, `ICommandList`, `IBuffer`, `ITexture`, `IPipelineState` 인터페이스로 상위 렌더러와 API 구현을 분리합니다.
- Vulkan/D3D12/Metal 구현 선택: CMake 옵션으로 플랫폼과 목적에 맞는 그래픽스 API 구현을 선택해 빌드합니다.
- 렌더링 계층 구성: `Renderer`, `Scene`, `Mesh`, `RenderPath`를 통해 장면 데이터, 모델, 재질, 조명, draw 제출 흐름을 관리합니다.
- 리소스 및 셰이더 바인딩 관리: texture, buffer, pipeline state를 생성하고 shader layout, descriptor binding, push constant 기반 데이터를 연결합니다.
- 모델 및 텍스처 로딩: OBJ, glTF, FBX 모델과 텍스처 리소스를 예제 렌더링 경로에서 검증합니다.
- 렌더링 제출 방식 비교: `07_RenderPath` 예제에서 per-draw, batched, bindless binding mode를 실행 인자로 전환할 수 있습니다.
- 그림자 렌더링: shadow map을 위한 별도 렌더링 경로와 조명 데이터를 처리합니다.
- SIMD 수학 경로: `DY_ENABLE_SIMD` 옵션으로 SIMD 기반 행렬 연산 경로를 선택하고 예제에서 차이를 확인할 수 있습니다.
- 예제 중심 빌드 검증: shader 복사/컴파일과 실행 파일 배치를 CMake post-build 단계에서 처리합니다.

## 프로젝트 자료

| 자료 | 링크 |
| --- | --- |
| 프로젝트 계획서 PPT | [추가 예정](docs/dy_engine_plan.pptx) |
| 프로젝트 완성 발표 PPT | [추가 예정](docs/dy_engine_prototype.pdf) |
| 시연 동영상 | [YouTube 링크 추가 예정](https://www.youtube.com/) |

## 팀원소개

| 이름 |  [![GitHub](https://img.shields.io/badge/한재승-181717?style=flat-square&logo=github)](https://github.com/suhanjin17)| [![GitHub](https://img.shields.io/badge/윤훈-181717?style=flat-square&logo=github)](https://github.com/yunhoon0206) | [![GitHub](https://img.shields.io/badge/정준혁-181717?style=flat-square&logo=github)](https://github.com/Wnsgur7318) |  [![GitHub](https://img.shields.io/badge/정현진-181717?style=flat-square&logo=github)](https://github.com/junghj0724)|
| :---: | :---: | :---: | :---: | :---: |
|역할 |Core/RHI|Vulkan|Metal|DirectX12|

## 설계 구조

```text
examples/       # 엔진 사용 예제와 렌더링 검증 코드
cmake/          # 빌드 옵션, API 선택, 외부 의존성 설정
src/
├─ RHI/          # 그래픽 API 독립 인터페이스
├─ Backends/     # Vulkan, D3D12, Metal 구현체
├─ Graphics/     # renderer, render path, scene, mesh
├─ Platform/     # window, input, time
├─ Math/         # 수학 유틸리티
├─ UI/           # UI 관련 타입
└─ Core/         # 공통 타입
```

### RHI 계층

`RHI::IDevice`, `ICommandList`, `IBuffer`, `ITexture`, `IPipelineState`를 기준으로 상위 renderer가 특정 그래픽 API에 직접 의존하지 않도록 구성합니다.

### Graphics 계층

`Renderer`, `Scene`, `Mesh`, `RenderPath`를 중심으로 모델, 재질, 조명, 렌더링 경로를 관리합니다. 상위 계층은 RHI 인터페이스를 통해 리소스와 command list를 사용하고, API별 호출은 직접 다루지 않습니다.

### API 구현 계층

그래픽스 API 구현은 CMake 옵션으로 선택합니다.

| 옵션 | 설명 |
| --- | --- |
| `USE_VULKAN=ON` | Vulkan 사용 |
| `USE_D3D12=ON` | Direct3D 12 사용 |
| `USE_METAL=ON` | Metal 사용 |

그래픽스 API 옵션은 한 번에 하나만 켜는 것을 기준으로 합니다. 현재 CMake 분기 우선순위는 `USE_D3D12`, `USE_METAL`, `USE_VULKAN` 순서입니다.

## 요구 사항

공통 요구 사항:

- CMake 3.20 이상
- C++17 지원 컴파일러
- Git

GLFW, stb, fastgltf, ufbx는 CMake `FetchContent`로 내려받습니다.

### Windows

- Visual Studio 2022 또는 MSVC C++ Build Tools
- Windows SDK
- Vulkan 사용 시 Vulkan SDK

### Linux

Ubuntu/Debian 기준 기본 패키지:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config
```

Vulkan 사용 시:

```bash
sudo apt-get install -y libvulkan-dev vulkan-tools vulkan-validationlayers-dev glslc
```

GLFW가 X11로 빌드될 때:

```bash
sudo apt-get install -y xorg-dev
```

Wayland를 사용할 때:

```bash
sudo apt-get install -y libwayland-dev wayland-protocols libxkbcommon-dev
```

### macOS

- Xcode 또는 Command Line Tools
- CMake

## CMake 옵션

| 옵션 | 기본값 | 설명 |
| --- | --- | --- |
| `USE_VULKAN` | `OFF` | Vulkan 사용 |
| `USE_D3D12` | `OFF` | Direct3D 12 사용. Windows 전용 |
| `USE_METAL` | `OFF` | Metal 사용. macOS 전용 |
| `DY_ENABLE_SIMD` | `ON` | 지원 CPU에서 SIMD 수학 경로 사용 |
| `DY_LINUX_WINDOW_SYSTEM` | `AUTO` | Linux GLFW window system 선택. `AUTO`, `X11`, `WAYLAND` |

## 빌드 방법

### Windows, Vulkan

```powershell
cd C:\GitHub\GitHub_Project\dy_engine
cmake -S . -B build-vulkan-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON -DDY_ENABLE_SIMD=ON
cmake --build build-vulkan-x64 --config Release
```

### Windows, Direct3D 12

```powershell
cd C:\GitHub\GitHub_Project\dy_engine
cmake -S . -B build-d3d12-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_D3D12=ON -DDY_ENABLE_SIMD=ON
cmake --build build-d3d12-x64 --config Release
```

### macOS, Metal

```bash
cd /path/to/dy_engine
cmake -S . -B build-metal -G Xcode -DUSE_METAL=ON -DDY_ENABLE_SIMD=ON
cmake --build build-metal --config Release
```

### Linux, Vulkan

```bash
cd /path/to/dy_engine
cmake -S . -B build-vulkan -DUSE_VULKAN=ON -DDY_ENABLE_SIMD=ON -DDY_LINUX_WINDOW_SYSTEM=AUTO -DCMAKE_BUILD_TYPE=Release
cmake --build build-vulkan
```

기존 `build` 폴더가 이미 같은 generator로 구성되어 있다면 configure 없이 build만 실행할 수 있습니다.

```powershell
cmake --build build --config Release
```

Generator/platform cache가 충돌하면 새 build 폴더를 사용합니다.

```powershell
cmake -S . -B build-new-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON -DDY_ENABLE_SIMD=ON
cmake --build build-new-x64 --config Release
```

SIMD 비교가 필요하면 같은 그래픽스 API 설정을 `-DDY_ENABLE_SIMD=OFF`로 다시 configure하여 별도 build 폴더에 빌드합니다.

## 실행 방법

예제는 실행 파일이 있는 폴더에서 실행해야 shader 상대 경로가 맞습니다.

```powershell
cd C:\GitHub\GitHub_Project\dy_engine\build-vulkan-x64\examples\03_Cube\Release
.\Cube.exe
```

Linux 단일 config 빌드에서는 보통 다음 경로에서 실행합니다.

```bash
cd /path/to/dy_engine/build-vulkan/examples/03_Cube
./Cube
```

대표 예제:

| 예제 | 실행 파일 |
| --- | --- |
| Hello Window | `examples/01_HelloWindow/Release/HelloWindow.exe` |
| Hello Renderer | `examples/02_HelloRenderer/Release/HelloRenderer.exe` |
| Cube | `examples/03_Cube/Release/Cube.exe` |
| Textured Cube | `examples/04_TexturedCube/Release/TexturedCube.exe` |
| Load Model | `examples/05_LoadModel/Release/LoadModel.exe` |
| Shadow Cube | `examples/06_ShadowCube/Release/ShadowCube.exe` |
| Render Path | `examples/07_RenderPath/Release/07_RenderPath.exe` |

`07_RenderPath`는 binding mode와 instance count를 실행 인자로 받을 수 있습니다.

```powershell
.\07_RenderPath.exe --per-draw --count=10000
.\07_RenderPath.exe --batched --count=10000
.\07_RenderPath.exe --bindless --count=10000
```

