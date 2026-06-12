# dy_engine

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus)
![CMake](https://img.shields.io/badge/CMake-3.20%2B-064F8C?style=flat-square&logo=cmake)
![Backend](https://img.shields.io/badge/Backends-Vulkan%20%7C%20D3D12%20%7C%20Metal%20%7C%20Null-555555?style=flat-square)
![Windows](https://img.shields.io/badge/Windows-MSVC-0078D4?style=flat-square&logo=windows)

`dy_engine`은 C++17과 CMake 기반의 렌더링 엔진 프로젝트입니다. 플랫폼과 그래픽 API 의존성을 RHI(Render Hardware Interface) 계층으로 분리하고, Vulkan/D3D12/Metal/Null backend를 선택적으로 붙일 수 있도록 설계했습니다.

## 목차

- [프로젝트 개요](#프로젝트-개요)
- [프로젝트 자료](#프로젝트-자료)
- [팀원 소개](#팀원소개)
- [주요 기능](#주요-기능)
- [설계 구조](#설계-구조)
- [빌드 방법](#빌드-방법)
- [실행 방법](#실행-방법)


## 프로젝트 개요

이 프로젝트의 목표는 렌더링 엔진의 기본 구조를 직접 설계하고, 실제 그래픽 API backend를 통해 화면 출력까지 연결하는 것입니다. 현재 코드는 예제 실행, shader 빌드, RHI 추상화, renderer 계층, backend별 device 구현을 포함합니다.

| 항목 |  | 
| --- | --- | 
| 언어 | ![Language](https://img.shields.io/badge/Language-C%2B%2B17-00599C?style=flat-square&logo=cplusplus) | 
| 빌드 시스템 | ![Build](https://img.shields.io/badge/Build-CMake-064F8C?style=flat-square&logo=cmake) 
| 렌더링 추상화 | ![RHI](https://img.shields.io/badge/RHI-abstracted-4B5563?style=flat-square) | 
| 그래픽 backend | ![Backends](https://img.shields.io/badge/Backends-Vulkan%20%7C%20D3D12%20%7C%20Metal%20%7C%20Null-555555?style=flat-square) | 
| 플랫폼 계층 | ![Window](https://img.shields.io/badge/Window-GLFW-7A7A7A?style=flat-square) | 
| 셰이더 | ![Shaders](https://img.shields.io/badge/Shaders-SPIR--V%20%7C%20HLSL-6B7280?style=flat-square) | 


## 프로젝트 자료

| 자료 | 링크 |
| --- | --- |
| 프로젝트 계획서 PPT | [추가 예정](readme/project-plan.pptx) |
| 프로젝트 완성 발표 PPT | [추가 예정](readme/final-presentation.pptx) |
| 시연 동영상 | [YouTube 링크 추가 예정](https://www.youtube.com/) |

## 팀원소개

| 이름 |  [![GitHub](https://img.shields.io/badge/한재승-181717?style=flat-square&logo=github)](https://github.com/suhanjin17)| [![GitHub](https://img.shields.io/badge/윤훈-181717?style=flat-square&logo=github)](https://github.com/yunhoon0206) | [![GitHub](https://img.shields.io/badge/정준혁-181717?style=flat-square&logo=github)](https://github.com/Wnsgur7318) |  [![GitHub](https://img.shields.io/badge/정현진-181717?style=flat-square&logo=github)](https://github.com/junghj0724)|
| :---: | :---: | :---: | :---: | :---: |
|역할 |RHI설계|Vulkan|Metal|Direct X|

## 주요 기능

- RHI 기반 graphics device 추상화
- Vulkan/D3D12/Metal/Null backend 선택 구조
- backend별 렌더링 경로 확장 구조
- shader layout 및 descriptor binding 관리
- texture, buffer, pipeline state 생성/해제
- shadow map rendering path
- 예제별 shader post-build 복사/컴파일
- Windows MSVC, CMake 기반 빌드

## 설계 구조

```text
src/
├─ RHI/          # 그래픽 API 독립 인터페이스
├─ Backends/     # Vulkan, D3D12, Metal, Null 구현체
├─ Graphics/     # renderer, render path, scene, mesh
├─ Platform/     # window, input, time
├─ Math/         # 수학 유틸리티
├─ UI/           # UI 관련 타입
└─ Core/         # 공통 타입
```

### RHI 계층

`RHI::IDevice`, `ICommandList`, `IBuffer`, `ITexture`, `IPipelineState`를 기준으로 상위 renderer가 특정 그래픽 API에 직접 의존하지 않도록 구성합니다.

### Backend 계층

Backend는 CMake 옵션으로 선택합니다.

| 옵션 | 설명 |
| --- | --- |
| `USE_VULKAN=ON` | Vulkan backend 사용 |
| `USE_D3D12=ON` | Direct3D 12 backend 사용 |
| `USE_METAL=ON` | Metal backend 사용 |
| 옵션 없음 | Null backend 사용 |

backend 옵션은 한 번에 하나만 켜는 것을 기준으로 합니다. 현재 CMake 분기 우선순위는 `USE_D3D12`, `USE_METAL`, `USE_VULKAN`, Null 순서입니다.

## 빌드 방법

### Windows, Vulkan

```powershell
cd C:\GitHub\GitHub_Project\dy_engine
cmake -S . -B build-vulkan-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON
cmake --build build-vulkan-x64 --config Debug
```

### Windows, Direct3D 12

```powershell
cd C:\GitHub\GitHub_Project\dy_engine
cmake -S . -B build-d3d12-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_D3D12=ON
cmake --build build-d3d12-x64 --config Debug
```

### Windows, Null backend

그래픽 API backend 없이 RHI/상위 구조를 빌드할 때 사용합니다.

```powershell
cd C:\GitHub\GitHub_Project\dy_engine
cmake -S . -B build-null-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-null-x64 --config Debug
```

### macOS, Metal

```bash
cd /path/to/dy_engine
cmake -S . -B build-metal -G Xcode -DUSE_METAL=ON
cmake --build build-metal --config Debug
```

### Linux, Vulkan

```bash
cd /path/to/dy_engine
cmake -S . -B build-vulkan -DUSE_VULKAN=ON
cmake --build build-vulkan --config Debug
```

기존 `build` 폴더가 이미 같은 generator로 구성되어 있다면 configure 없이 build만 실행할 수 있습니다.

```powershell
cmake --build build --config Debug
```

Generator/platform cache가 충돌하면 새 build 폴더를 사용합니다.

```powershell
cmake -S . -B build-new-x64 -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON
cmake --build build-new-x64 --config Debug
```

## 실행 방법

예제는 실행 파일이 있는 폴더에서 실행해야 shader 상대 경로가 맞습니다.

```powershell
cd C:\GitHub\GitHub_Project\dy_engine\build-x64\examples\03_Cube\Debug
.\Cube.exe
```

대표 예제:

| 예제 | 실행 파일 |
| --- | --- |
| Hello Window | `examples/01_HelloWindow/Debug/HelloWindow.exe` |
| Hello Renderer | `examples/02_HelloRenderer/Debug/HelloRenderer.exe` |
| Cube | `examples/03_Cube/Debug/Cube.exe` |
| Textured Cube | `examples/04_TexturedCube/Debug/TexturedCube.exe` |
| Load Model | `examples/05_LoadModel/Debug/LoadModel.exe` |
| Shadow Cube | `examples/06_ShadowCube/Debug/ShadowCube.exe` |
| Render Path | `examples/07_RenderPath/Debug/07_RenderPath.exe` |


