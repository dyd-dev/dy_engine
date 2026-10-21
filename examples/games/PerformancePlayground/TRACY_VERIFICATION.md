# 실제 게임 Tracy 계측 검증

이 문서는 이전 `execution-visualizer` 브랜치의 역사적 캡처 기록이다. 현재 main API 빌드는 공개 TracyAdapter와 게임 힙 계측을 사용하며 아래의 큐 GPU 타임라인/락/엔진 호출 스택 결과를 재검증했다고 주장하지 않는다. 현재 결과는 [MAIN_API_VERIFICATION.md](MAIN_API_VERIFICATION.md)에 기록한다.

2026-10-17, Windows / NVIDIA GeForce RTX 4070 Laptop GPU / Vulkan 1.4.341 / Tracy 0.13.1.

## 최종 CPU 제출 시간 보정 검증 — 2026-10-18

남아 있던 보정을 포함해 Release 게임 전체를 최종 재빌드했고 CTest 4/4가 통과했다. 새 실행 파일로 Vulkan validation을 켜고 실제 P 입력으로 병렬 기록을 활성화한 뒤 원본 Tracy 수집기로 12초 캡처를 받았다. CPU 시작/종료 시각은 `vkQueueSubmit` 바로 앞뒤에서 취득하며, 제출 성공 후 해당 시각을 GPU zone 이벤트에 보존한다.

최종 게임 SHA256: `A9C5CB959DCAB24F48B26B12BFF6C51AB151DF52FB8BE449DFD16AAADB3AFE96`.
최종 캡처 SHA256: `EE4124C9BBA3B55DE25895694B4B424EBCA519DB84495C0A6EDDDE4C6C49120A`.

| 최종 실제 게임 캡처 | 값 |
|---|---:|
| 캡처 프레임 / 시간 범위 | 538 / 12.15 s |
| 완료된 Vulkan GPU 제출 구간 | 1,067 |
| CPU 제출 호출 평균 | 0.147168688 ms |
| GPU 제출 구간 평균 | 2.746119987 ms |
| C++ 할당 / 해제 | 1,177,229 / 1,144,579 |
| 할당 호출 스택 | 1,177,229 |
| 작업 큐 락 이벤트 / 참여 스레드 | 32,046 / 3 |
| Renderer / worker CPU 호출 스택 | 535 / 3,204 |

`ViewerTracyGameCaptureTests`가 모든 완료 GPU 구간의 양수 CPU 제출 시간과 GPU/heap/lock/stack 데이터를 확인해 통과했다. 별도로 원본 `tracy-csvexport -g`의 GPU 이벤트 1,067개를 합산·평균한 값은 2.7461199868791 ms로 검사기 출력과 반올림 오차 내에서 일치했다.

내장 Tracy GUI에서 최종 캡처를 다시 열어 `Vulkan queue submission` 상세를 확인했다. 시작 시각 33,451,014,987 ns의 구간은 **CPU command setup time 175.93 μs**, **GPU execution time 5.34 ms**로 표시됐다. 같은 GPU 구간의 원본 CSV 값은 5,335,313 ns(5.335313 ms)다. 이 CPU 값은 이전의 이벤트 발행 비용이 아니라 실제 `vkQueueSubmit` 호출 범위이며, 전체 프레임 CPU 시간과 구분한다.

최종 게임 로그의 Vulkan validation 오류는 0개다. 외부 overlay layer의 API 버전 경고 2개와 소비되지 않는 vertex attribute 경고 3개는 남아 있다. 게임과 최종 저장 캡처를 연 분석기는 검증 후 열어 두었다. 저장본을 연 분석기의 Overview는 라이브 연결이 없어 미수집으로 보일 수 있으며, 아래 Tracy 탭에서 저장된 측정값을 확인한다.

증거 폴더: [`build/tracy-instrumented-qa/final-20261018/`](../../../build/tracy-instrumented-qa/final-20261018/).
원본: [`final-game.tracy`](../../../build/tracy-instrumented-qa/final-20261018/final-game.tracy).
기계 판독 결과·실행/소스 해시·GUI 표본: [`verification.json`](../../../build/tracy-instrumented-qa/final-20261018/verification.json).
빌드·테스트·캡처 로그와 `gpu-events.csv`, `gpu-independent-check.json`도 같은 폴더에 보존했다.

## 계측 범위

- Vulkan graphics queue에 실제로 제출하는 명령 배치 앞뒤에서 native timestamp를 쓴다. 명령 폐기와 제출 전 검증 실패에는 쿼리를 할당하지 않는다. marker command pool은 해당 제출의 fence와 함께 회수한다.
- Tracy의 기존 Vulkan query collector와 CPU/GPU calibration을 재사용한다. 재연결 시 이전 제출 완료를 기다린 뒤 context를 교체한다. GPU 종료 이벤트는 시작 이벤트와 같은 실제 제출 스레드를 사용한다.
- GPU 구간의 CPU 시간은 실제 `vkQueueSubmit` 호출 구간이다. GPU 시간과 CPU 시간을 서로 대신 사용하지 않는다. Overview GPU coverage는 다른 측정 범위이므로 기존 엔진 값을 유지한다.
- `Game C++ heap`은 게임 EXE에 연결된 C++ new/delete 계측이다. 배열, sized delete, nothrow, over-aligned 할당을 포함한다. 연결 이후 이벤트를 관측하며 전체 프로세스 메모리나 VRAM을 의미하지 않는다.
- `ThreadPool task queue`는 실제 작업 큐의 mutex다. 게임에서 P로 parallel recording을 켜 실제 두 worker와 main thread의 이벤트를 수집했다.
- Renderer와 worker CPU 구간, heap 할당에 8단계 stack을 수집한다. MSVC Release에 PDB를 생성하여 실제 함수·파일·줄을 해석한다.

## 네이티브 증거

작업 산출물은 저장소의 `build/tracy-instrumented-qa/`에 있다. 합성 fixture가 아닌 PerformancePlayground 실행에서 수집했다.

초기 실게임 캡처 `game-fixed.tracy` 검사 결과:

| 항목 | 원본 캡처 값 |
|---|---:|
| 완료된 Vulkan GPU 구간 | 2,608 |
| GPU 제출 구간 평균 | 2.590308737 ms |
| C++ 할당 / 해제 | 3,915,720 / 3,897,703 |
| 호출 스택이 있는 할당 | 3,915,720 |
| 작업 큐 락 이벤트 / 참여 스레드 | 78,297 / 3 |
| Renderer CPU 호출 스택 / worker CPU 호출 스택 | 1,305 / 7,830 |
| calibration된 Vulkan context | 1 |

이 캡처의 게임 SHA256은 `B707C6379130CB8FB2AA29F6B404A1C0E496FD3C3989769DAAF5CDB093379510`, 캡처 SHA256은 `A5042D42843CBA06652538BD68A98BD21C8CC782FF5CD90F2D3F7062603819A5`다. GPU 이벤트 CPU 시간 범위를 최종 보정하기 전의 캡처이며, GPU timestamp와 heap/lock/stack 데이터의 원본 증거로 보존한다.

같은 게임에 원본 `tracy-capture.exe`로 다시 연결한 8초 `game-reconnect.tracy`도 검사 통과: GPU 701개, 할당 773,929개, 해제 748,104개, 락 21,114개/3 threads, Renderer stacks 351개, worker stacks 2,112개. SHA256 `82ECFBEB67E27FA70B320C1EF14B8D4E9815343A545786B17224583AC72CD482`.

GUI에서 GPU 제출 구간 상세의 실제 GPU 5.37 ms, Memory → Game C++ heap의 3,915,720개 할당과 활성 할당 18,017개, Memory 호출 스택 트리의 `ReadLiveSnapshot`, `SetInlineConstantsNative`, `AcquireCommandList` 등 실제 엔진 함수·소스 줄을 확인했다. 라이브 캡처 Frame 카운터는 735에서 1,179로 증가했고 저장본은 1,307 frames다.

Vulkan validation을 켜서 실행했다. 검사한 게임 로그에 validation error/VUID error는 없었다. 기존 외부 overlay layer의 API 버전 경고 2개와 shader가 소비하지 않는 vertex attribute 경고 3개는 남아 있다.

## 검사 구분

- 이전 게임 캡처에는 GPU/heap/lock/callstack이 모두 없어 새 검사기가 실패했다.
- `ViewerTracyGameCaptureTests`는 저장 파일을 Tracy의 Worker로 다시 읽어 실제 GPU 완료 시간, heap alloc/free, 2개 이상 스레드의 큐 락, 엔진 심볼, Renderer 및 worker stack을 검사한다.
- Tracy ON CTest 4/4: 입력, allocator 규약, 실제 Vulkan RenderGraph/ThreadPool 회귀, 에셋. `ctest-tracy-on.log` 보존.
- Tracy OFF 최신 Vulkan 빌드의 기존 `RenderGraphTests` 통과: worker overlap, 예외, self-wait 거부, 작업 종료, 기록 순서, 자원 수명, 실패 복구, GPU 업로드와 fence 검사.
- 최초 GPU 종료 이벤트의 C API는 thread 0을 전송해 원본/내장 Tracy 수집기 모두 종료됐다. 원본 Vulkan 구현과 같은 thread ID로 고친 뒤 위 두 실제 캡처가 통과했다. 실패 로그도 보존했다.

계측 부하를 포함한 진단 실행이다. 성능 향상 비교, 모든 드라이버/장치, 메모리 부족 및 device-lost 강제 주입까지 검증한 결과는 아니다.

## RenderDoc 이식 조사

현재 `RenderDocCapture.cpp`에는 캡처 요청과 외부 Replay UI 실행 경로가 있다. 이는 게임에 주입된 RenderDoc의 in-application API를 사용하는 기능이다. [공식 API 문서](https://github.com/baldurk/renderdoc/blob/v1.x/docs/in_application_api.rst).

분석 결과를 현재 ImGui 분석기 안에 표시하는 방식은 가능하다고 판단한다. Replay API의 `GetRootActions`, `GetResources`, `GetTextures`, `GetBufferData`, `GetTextureData`, `SetFrameEvent`로 캡처 이벤트·자원·텍스처·버퍼·파이프라인 정보를 읽을 수 있다. native window용 `CreateOutput`도 제공한다. [공식 Replay API](https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/api/replay/renderdoc_replay.h).

원본 RenderDoc UI 전체는 Qt QApplication/MainWindow/CaptureContext에 의존한다. 현재 Tracy처럼 ImGui draw 함수를 직접 호출하는 방식으로 넣을 수는 없으며, Qt UI·이벤트 루프·도킹·출력 window 수명을 통합하거나 필요한 화면을 ImGui로 다시 구현해야 한다. 첫 단계로 기존 캡처 제어를 재사용하고 Replay API로 이벤트 목록·선택 이벤트의 상태·텍스처 미리보기를 가져오는 범위가 작다. 이는 소스/API 조사에 따른 설계 판단이며, 이번 작업에서 RenderDoc Replay UI를 구현하거나 임베딩 실행을 검증하지 않았다. [원본 Qt 진입 코드](https://github.com/baldurk/renderdoc/blob/v1.x/qrenderdoc/Code/qrenderdoc.cpp).
