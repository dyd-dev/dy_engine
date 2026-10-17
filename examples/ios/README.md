# advanced 07·08 iOS 실행

`AdvancedMetalIOS/AdvancedMetalIOS.xcodeproj`를 Xcode에서 연다. 이 프로젝트는
`examples/advanced/07_PBRLighting`과 `08_ShadowMapping`의 원본 C++ 렌더링 코드와
Metal 셰이더를 함께 사용한다. dyf 예제를 대신 실행하는 프로젝트가 아니다.

## 실행 방법

1. Xcode에서 위 프로젝트를 연다.
2. 실행 구성을 `Advanced07_PBRLighting` 또는 `Advanced08_ShadowMapping`으로 선택한다.
3. 실행 대상을 연결한 iPhone/iPad로 선택한다.
4. `Signing & Capabilities`에서 자신의 개발 Team을 선택한다. 다른 팀원이 사용할 때는
   필요하면 Bundle Identifier도 자신만의 값으로 변경한다.
5. 실행 버튼을 누른다. 엔진과 셰이더도 빌드 단계에서 자동으로 갱신된다.

앱은 가로 화면으로 실행된다. 휴대폰을 옆으로 돌려서 보고, 화면 위의
`07 PBR` / `08 그림자` 버튼을 누르면 앱 안에서 바로 예제를 바꿀 수 있다.
전환할 때 기존 GPU 작업을 마무리하고 리소스를 해제한 뒤 새 예제를 만든다.
버튼으로 선택하면 해당 예제의 기본 설정으로 계속 실행한다.
Xcode 실행 인자는 처음 실행할 때만 적용하며 다른 예제로 넘기지 않는다.

필요 도구는 Xcode의 iOS SDK와 Metal Toolchain, CMake 3.20 이상, Ninja다.
첫 빌드에서는 기존 엔진 의존성인 stb를 내려받기 때문에 인터넷 연결이 필요하다.
이후 엔진/예제 바이너리는 저장소의 무시된 `build/ios-<SDK>-<CPU>-<구성>`에 생성된다.
소스와 실행 파일에 개인의 절대 경로를 고정하지 않는다.

최소 iOS 버전은 17.0으로 설정했다. 이는 모든 iOS 17 기기를 검증했다는 뜻은 아니다.
실기기와 시뮬레이터는 서로 다른 라이브러리·Metal 바이너리를 생성한다.

## 설정값 변경

Xcode의 `Product > Scheme > Edit Scheme > Run > Arguments`에서 실행 인자를 추가한다.
선택한 실행 구성에 `--example 07` 또는 `--example 08`이 이미 들어 있다.
옵션 이름과 값을 각각 한 항목씩 추가한다. 예: `--roughness`와 `0.2`.

| 예제 | 옵션 | 허용값 / 기본값 |
|---|---|---|
| 공통 | `--frames` | 0 이상 정수 / 0(계속 실행) |
| 공통 | `--time` | 0~1,000,000초 / 생략 시 프레임 수÷60 |
| 공통 | `--exposure` | 0.01~20 / 1 |
| 공통 | `--capture` | PPM 파일 경로 / 생략 시 저장 안 함 |
| 07 | `--mode` | pbr / lambert / normal, 기본 pbr |
| 07 | `--roughness` | 0.045~1 / 0.45 |
| 07 | `--metallic` | 0~1 / 0.8 |
| 08 | `--shadow-type` | directional / cascaded / point, 기본 directional |
| 08 | `--mode` | shadow / unshadowed / depth / visibility / regions, 기본 shadow |
| 08 | `--bias` | 0~0.1 / 0.0015 |
| 08 | `--pcf` | 정수 0~2 / 1 |
| 08 | `--split` | 4~16 / 8 |
| 08 | `--light-height` | 2~8 / 4 |

`--capture result.ppm`처럼 상대 경로를 쓰면 앱의 Documents 폴더에 저장한다.
캡처를 요청하면서 `--frames`를 생략하면 1프레임만 그린다.
최신 main에서 제거된 07의 `--capture`는 iOS 결과 검증과 08의 공통 실행 경로를 위해 유지했다.
`--frames`가 끝나면 iOS 앱을 강제로 종료하지 않고 마지막 화면에서 갱신을 멈춘다.
`--help`는 Xcode 콘솔에 도움말을 출력한다.

기존 `--validation`은 RHI 장치 검증 기능 요청이다. Metal 백엔드는 이 요청을
지원하지 않으므로 추가하지 않는다. Metal API 검증은 Xcode Run 설정의
Diagnostics에서 사용하는 별도의 기능이다.

가로 화면의 실제 픽셀 크기로 그리므로 구체가 늘어나거나 양옆이 잘리는 세로 구도를 피한다.
가로 방향 전용 앱으로 전체 화면을 요청한다. iPad의 분할 화면은 이번 지원 범위에 포함하지 않는다.
앱 화면의 선택 버튼은 07·08 전환용이다. FPS 표시나 설정값 조절 슬라이더는 없으며,
세부 설정은 Xcode 실행 인자를 변경한 뒤 다시 실행한다.
CADisplayLink의 60 FPS 설정은 목표 호출 주기이며 실제 측정된 GPU 성능이 아니다.

## 코드가 연결되는 순서

```text
ViewController.mm
  ├─ MetalView.layer → CAMetalLayer
  ├─ --example 07 → CreatePBRLighting()
  └─ --example 08 → CreateShadowMapping()
       ↓
common/Example.h : Tick()
  BeginFrame → 예제의 Draw() → 캡처(선택) → Present
       ↓
MetalDevice / MetalPipeline / MetalCommandList
       ↓
iPhone GPU → MetalView 화면
```

- `main.cpp`의 옵션 파서·렌더링 명령은 데스크톱과 iOS에서 공유한다.
  데스크톱에서는 Window의 이벤트 반복문이 `Tick()`을 호출한다.
  iOS에서는 UIKit의 `CADisplayLink`가 같은 함수를 호출한다.
- 07은 기존 PBR 전체 화면 패스를 사용한다.
- 08은 기존 1536×1024 그림자 깊이 패스와 최종 조명 패스를 사용한다.
- 셰이더 수식, 기본 설정값, 바이어스·PCF·그림자 종류의 의미는 변경하지 않았다.
- 화면 회전은 UIView의 배율·크기를 읽어 스왑체인의 픽셀 크기에 반영한다.
  앱이 비활성화되면 콜백을 멈추고 제출된 GPU 작업이 끝나기를 기다린다.
- 오류는 콘솔과 앱의 오류 문구로 표시한다. 검은 화면만 남기지 않도록 했다.

## 파일별 변경 이유

| 파일 | 변경과 이유 |
|---|---|
| 루트 `CMakeLists.txt`, `cmake/Dependencies.cmake` | iOS에서는 GLFW와 데스크톱 Window.cpp를 빌드하지 않는다. |
| `src/Backends/Metal/CMakeLists.txt` | iOS는 UIKit, macOS는 AppKit을 연결한다. |
| `MetalDevice.mm` | iOS 기본 GPU 선택, UIView의 기존 CAMetalLayer 사용, 픽셀 크기 갱신, Fifo 표시만 허용한다. 맥 전용 API는 macOS 분기에 둔다. |
| `MetalTexture.mm`, `MetalPipeline.mm` | iOS SDK에 없는 D24S8 네이티브 상수·조회 API를 제외한다. D32/D32S8 경로와 macOS D24S8 경로는 유지한다. |
| `cmake/MetalToolchain.cmake` | SDK와 AIR 타깃을 iOS 실기기/시뮬레이터에 맞춘다. 맥용 metallib를 아이폰에 넣지 않는다. |
| `CompileShaders.cmake`, `ShaderAssets.cmake` | 위 타깃 옵션을 예제·엔진 내장 셰이더의 컴파일에 전달한다. |
| `examples/advanced/common/Example.h` | 두 예제가 공유하는 장치·스왑체인 수명, 한 프레임 실행, 캡처 기능을 모았다. |
| 07·08의 `main.cpp` | 초기화와 프레임 그리기를 Renderer로 분리했다. 각 예제의 셰이더 데이터는 파일 내부 이름 공간에 넣어 한 앱에 링크할 때 충돌하지 않게 했다. |
| 07·08의 `CMakeLists.txt` | 데스크톱은 실행 파일, iOS는 앱에서 호출할 정적 라이브러리로 빌드한다. |
| `examples/ios/CMakeLists.txt`, `build-libraries.sh` | Xcode의 SDK·CPU·Debug/Release 설정으로 엔진과 두 예제를 빌드한다. 이번 앱에서 사용하지 않는 Tracy/Model/ImGui 확장은 끈다. |
| `ViewController.mm` | UIView와 엔진을 연결하고 프레임 호출·중단·재개 및 실행 인자를 처리한다. 가로 화면을 요청하고 07·08 선택 버튼으로 렌더러를 교체한다. |
| Xcode 프로젝트·공유 Scheme | 실행 버튼으로 의존성 빌드/링크가 되도록 연결하고 07·08을 선택할 수 있게 했다. CMake가 소스/빌드 폴더에 접근해야 하므로 해당 프로젝트의 User Script Sandboxing을 껐다. |

Windows/Vulkan/D3D12 백엔드 코드는 수정하지 않았다. 하지만 두 예제의 공통 C++
구조는 바뀌었으므로 해당 플랫폼의 최종 검증은 그 환경의 빌드/실행이 필요하다.
이 포팅은 advanced 07·08을 위한 것이며 모든 dyf/advanced 예제의 iOS 지원을 의미하지 않는다.

## 확인한 결과 (2026-10-16)

- Xcode 27.0에서 iOS arm64 엔진·예제 라이브러리, 서명 없는 앱 및 개발 서명 앱 빌드 성공.
- 연결된 **iPhone 15 Pro / iOS 27.0.1**에 앱 설치 후 Metal API Validation을 켜고 실행.
- 아래 9조건 모두 3프레임 완료 및 **1179×2556 GPU readback** 성공.
- macOS에서도 동일 9조건을 실행했고, 변경 전후 고정 시간 캡처가 **바이트 단위로 동일**했다.
- PBR·방향광·점광원 캡처를 열어 실제 구체/바닥/조명/그림자 출력을 확인했다.
- 가로 화면 대응 후 07의 **2556×1179 GPU readback**으로 구체 세 개가 모두 보이는 것을 확인했다.
- 같은 아이폰에서 화면의 선택 버튼을 눌러 **07→08→07** 전환을 확인했다.
  Metal API Validation을 켠 상태에서 그림자 장면과 PBR 장면이 정상 표시되었다.
- `git diff --check`, 빌드 스크립트 문법 및 Xcode 프로젝트 plist 검사 통과.

| 예제 | 실제 실행한 조건 |
|---|---|
| 07 | pbr, normal, lambert(roughness 0.2 / metallic 0.1 / exposure 1.5) |
| 08 | directional/shadow, cascaded/regions, point/shadow, depth, visibility, unshadowed |

로컬 결과 요약·캡처·로그: 저장소의 `build/ios-verification/`.

직접 검증하지 않은 항목은 iPad 실기기, iOS 시뮬레이터, 실제 기기 회전 및
백그라운드 전환의 수동 조작, Windows/Vulkan/D3D12 실행이다.
프레임 3회 검증은 장시간 안정성이나 성능 측정을 의미하지 않는다.

아래 백업과 검증 산출물은 작업한 컴퓨터에만 있으며 저장소에는 포함하지 않는다.

작업 직전 소스 백업은 `backups/ios-port-before-20261016-143029/`에 있다.
수동으로 작업하던 iOS 코드와 기존 07·08 코드를 함께 보관했다.
가로 화면·선택 버튼 추가 직전의 앱 파일은 `backups/ios-before-landscape-switch/`에 있다.

## 최신 main 통합 확인 (2026-10-17)

`89ac61c` 기준으로 iOS 변경을 통합한 뒤 아래 항목을 다시 확인했다.

- iOS arm64 Debug 앱 빌드 성공(코드 서명 제외).
- macOS Debug의 07·08 빌드 성공.
- 위 9조건을 macOS에서 Metal API Validation을 켜고 각각 3프레임 실행해 모두 종료 코드 0과 PPM 저장 확인.
- 통합 후 iPhone 실기기 재실행은 하지 않았다. 위 실기기 결과는 통합 전 검증이다.
