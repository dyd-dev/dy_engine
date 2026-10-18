# 프로파일링과 프레임 캡처

측정은 `src/Platform/Profiler`에서 처리합니다. Renderer·RHI·Canvas·Model이 같은 시계와
CPU 구간을 사용하고, HUD·실행 분석기·Tracy는 그 결과를 소비합니다.
GPU 질의의 수명과 프레임 귀속은 `FrameProfiler`와 `RhiProfiler`가 관리하며,
`src/dyf/ProfilerOverlay.cpp`는 결과를 그리는 역할만 맡습니다.

## 공개 API

공개 진입점은 `dyf/Platform/Profiler.h` 하나입니다. `dyf::Platform::Profiler`가
CPU 프레임과 완료된 GPU 표본을 집계하고 `GetSnapshot()`으로 결과를 제공합니다.
Renderer의 HUD와 `GetProfilerTimingSnapshot()`도 이 집계를 사용합니다.
측정 구현은 `src/Platform/Profiler/Profiler.cpp`에 있습니다.

상세 수집·이벤트·제어는 비공개 `CollectorInternal.h`, 프로세스 간 통신과
`ProfilerClient`는 비공개 `Connection/Protocol.h`에 둡니다.
HUD를 쓰는 예제와 showcase는 외부 클라이언트나 연결 세션을 생성하지 않습니다.

## Tracy와 RenderDoc

외부 SDK 어댑터는 도구별로 분리합니다.

| 도구 | 공개 헤더 | CMake 타깃 | 구현 |
| --- | --- | --- | --- |
| Tracy | `dyf/Extends/Tracy.h` | `dyf::Tracy` | `src/Extends/Tracy` |
| RenderDoc | `dyf/Extends/RenderDoc.h` | `dyf::RenderDoc` | `src/Extends/RenderDoc` |

앱은 필요한 어댑터만 링크합니다. Tracy를 사용할 때는 장치와 렌더러보다 먼저
`dyf::TracyAdapter`를 생성하고, 계측 작업이 모두 끝난 뒤 파괴합니다.
어댑터는 코어가 관측한 CPU 구간과 발행한 시간·자원 통계를 전달합니다.
자체 시계나 별도 프레임 집계를 만들지 않습니다.

`DY_ENABLE_TRACY`는 SDK 없는 `DY_PROFILER_TRACING_ENABLED` 정의를 코어에 전달합니다.
Tracy SDK 헤더와 SDK 전용 정의는 Tracy 어댑터 내부에서만 사용합니다.
옵션을 끄면 외부 도구 전용 구간 라벨·매크로 인자·전달 호출이 제외됩니다.

RenderDoc은 `DY_ENABLE_RENDERDOC=ON`과 `renderdoc_app.h`가 필요합니다.
`DY_RENDERDOC_ROOT`에 설치 또는 소스 경로를 지정할 수 있습니다.
RenderDoc으로 앱을 실행한 뒤 `dyf::RenderDocCapture::TriggerNextFrame()`을
호출하면 다음 프레임을 캡처합니다. 이미 주입된 API만 사용하며 도구를 직접 실행하지 않습니다.
Metal에서는 Xcode GPU Capture를 사용합니다.

D3D12에서는 같은 옵션으로 WinPixEventRuntime 이벤트·마커도 활성화합니다.
옵션을 끄면 런타임을 다운로드하거나 링크하지 않습니다. 켜진 구성은 MSVC가 필요하며,
패키징할 때는 해당 런타임 DLL을 실행 파일 옆에 배치해야 합니다.

## 예제 통합 실행기

`DY_BUILD_EXAMPLES=ON`이면 빌드 루트에 `visualizer`가 생성됩니다.
인자 없이 실행하면 GUI에서 dyf·RHI·advanced·showcase 분류를 고르고, 목록을 스크롤하며 예제를 클릭해 실행합니다.
Windows·Linux는 별도 실행 파일을, Android·iOS 앱은 같은 목록에 등록된 예제 함수를 앱 안에서 실행합니다.
모바일은 선택 화면의 GPU·Window 자원을 해제한 뒤 예제를 실행하고, 돌아가기 동작으로 목록을 다시 엽니다.
`visualizer --list`로 목록을 출력하고, `visualizer AdvancedProfiling --frames 120`처럼
개별 예제의 인자를 전달할 수도 있습니다. 예제에 필요한 자산 경로는 실행기가 맞춥니다.
계측 테스트 실행 파일은 예제 목록에 포함하지 않습니다.

HUD는 상세 수집이나 프로세스 간 연결 없이 동작합니다.
`DY_ENABLE_LIVE_PROFILING`은 내부 분석용 추가 수집 옵션이며,
비공개 `Connection` 구현은 HUD와 예제 실행기에 필요하지 않습니다.

이전 실행 분석기의 모든 화면이 Profiler로 통합된 것은 아닙니다. 현재 HUD는 프레임·CPU/GPU 시간,
리소스 수와 추이를 표시합니다. 상세 이벤트·객체·연결·제어·전후 비교의 수집 기반은 비공개 구현에 남아 있지만,
이전 Overview/History, Events/Objects/GPU/Details, Flow 그래프·표본 재생, Controls·Comparison 화면의 원본은
[`visualizer/`](../visualizer/README.md)에 보관하며 현재 빌드에는 연결하지 않습니다.
공개 `Profiler`는 시간 표본 집계와 스냅샷을 제공하며 이 분석 화면들을 제공하지 않습니다.

## 예제의 조작 정책

각 예제와 showcase가 사용하는 외부 도구 어댑터의 수명을 직접 관리합니다.
F11의 HUD 조작과 F12의 RenderDoc 요청도 해당 예제에서 처리합니다.
키 바인딩과 Renderer 설정 변경은 공개 프로파일러 라이브러리의 책임이 아닙니다.
Android와 iOS 진입점은 각각 최상위 `android/`, `ios/`에 있습니다.

사용자가 확인할 항목은 CPU/GPU 측정의 프레임 귀속, HUD·Tracy의 같은 수치,
그리고 RenderDoc 캡처입니다.
