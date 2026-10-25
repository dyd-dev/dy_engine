# dy_engine

[문서 · 시작하기 · API](https://dyd-dev.github.io/dy_engine/index.html) 

## CMake 빌드 옵션

| 옵션 | 엔진 기본값 | 설명 |
| --- | --- | --- |
| `USE_VULKAN` | `OFF` (Windows) | Windows에서 Vulkan 선택 |
| `USE_D3D12` | `OFF` (Windows)| Windows에서 Direct3D 12 선택 |
| `DY_BUILD_EXAMPLES` | `OFF` | 데스크톱 단독 예제 빌드 |
| `DY_BUILD_SHOWCASE` | `DY_BUILD_EXAMPLES` 값 | 발표용 장면 빌드·에셋 준비, Model·AdvancedRendering 자동 활성화 |
| `DY_EXTEND_MODEL` | `OFF` | 모델 로딩·애니메이션 확장 |
| `DY_EXTEND_ADVANCED_RENDERING` | `OFF` | 고급 렌더링 확장 |
| `DY_EXTEND_IMGUI` | `OFF` | 데스크톱 ImGui 확장 |
| `DY_ENABLE_SIMD` | `ON` | SIMD 연산 |
| `DY_ENABLE_TRACY` | `ON` | Tracy 프로파일링 연결 |
| `DY_ENABLE_LIVE_PROFILING` | `OFF` | 실행 진단 수집 |
| `DY_ENABLE_RENDERDOC` | `OFF` | RenderDoc 연결 |
| `DY_LINUX_WINDOW_SYSTEM` | `AUTO` | Linux 창 시스템: `AUTO`·`X11`·`WAYLAND` |

## CI

| 플랫폼 | 빌드 |
| --- | --- |
| Linux | Vulkan Debug/Release |
| Windows | Vulkan·Direct3D 12 Debug/Release |
| macOS | Metal Debug/Release |
| Android | Debug/Release APK, arm64-v8a·x86_64 |
| iOS | Debug/Release, 실기기·시뮬레이터 arm64 |

[CI 안내](.github/ci/README.md)
