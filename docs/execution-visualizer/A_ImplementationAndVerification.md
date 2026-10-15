# A 구현 및 검증 — 2026-10-15

담당 A의 공통 계측·전송과 상위 Renderer/Model 연동에 최적화 분석 protocol v3를 추가했다. 현재 계약은 `SharedContracts.md`와 공개 헤더를 따른다. 아래 v2·추가 기능·callback/GLFW/Native 설명, 수치와 hash는 각 단계 당시 이력이며 이번 v3의 실행 증거로 재사용하지 않는다.

## 최적화 분석 v3 — 현재 단계

RendererConfig의 6개 정책과 ModelOptimizationConfig의 4개 정책을 실제 frame boundary에서 선택한다. 기본값은 모두 true여서 기존 동작을 유지한다. Renderer는 증분 transform 업로드, ResourceSet 재사용, stock bindless/기존 non-bindless 파이프라인을 전환한다. Model은 완료된 frame slot 재사용, 같은 바이트 업로드 생략, immutable compute 입력 메시, influence packing/검증 캐시를 전환한다. OFF도 completion wait, 매 엔티티 palette 검증, 입력 순서, 자원 상태, 실패 rollback과 RHI 지연 회수를 유지한다. Model public GetOptimizationConfig는 대기 선택, GetAppliedOptimizationConfig는 실제 현재 정책이다.

Canvas의 3개 공개 정책·등록·적용값은 A가 연결했다. Canvas 소비부와 native 하네스 2개 파일은 구현 초안 뒤 B에게 이관했다. 배칭 OFF는 원래 순서의 draw를 내고, 이미지 캐시 OFF는 이전 프레임 항목 조회를 생략하되 프레임 내 dedup과 실패 rollback을 유지하며, VB 재사용 OFF는 새 버퍼를 transaction에 넣는다. 최종 Canvas 검증은 B/Root 증거를 따른다.

기존·신규 control에 생산자 소유 category/description/onPath/offPath/conditions/tradeoffs를 제공한다. Scene component 배열, build-time SIMD, geometry/texture/constant/shadow-bounds 재사용 및 Model descriptor/lazy-command/shared-palette/zero-morph 항목은 실제 조건과 runtime 대조 경로 부재를 설명하는 읽기 전용 목록이다. 존재하지 않는 AoS/SoA 전환이나 animation dirty-subtree 경로는 만들거나 광고하지 않는다. ModelScene에서 Render 바깥에 수행한 animation/morph 작업은 현재 render frame 비용으로 귀속하지 않는다.

Model/custom geometry 및 instance stream 부재에서는 transform 제어가 비활성화된다. Bindless 지원 판정은 허용 flag와 독립된 실제 stock fragment/layout 지원을 사용하므로 OFF에서 다시 ON이 가능하다. VS 모드의 static compute mesh cache도 사유가 있는 비활성 상태이며 Compute 전환 뒤 활성화된다. 미지원 native bindless는 사유 있는 skip으로 기록한다.

ControlStatus.previousActual은 적용 직전 실제 descriptor 값이다. Source의 최신 Applied request 한 개만 완료 1초 구간과 일치하는 before/after 원본 frame을 보존한다. 현재/직전 초의 첫 ended frame 후보를 초당 한 번만 독립 복사한다. 각 record는 기존 32MiB 중 optional 24MiB 안에서 1MiB를 RAII 예약하며 events 2048, links 4096, GPU spans 128, object metadata 128개와 label 256바이트의 모든 capacity를 포함한다. 예산 부재·prefix 초과·누락은 available/budgetLimited와 local loss로 제공한다. 종료 source 및 만료된 비고정 후보를 해제한다. 정상 late event/link는 예약된 범위에 추가하고 완료 counter/GPU 결과는 같은 원본 key에 보정한다. revision이 섞인 후보를 재명명하지 않고 비교 불가로 둔다. 프로그램에서 revision을 바꿔도 기존 status의 고정 after와 일치하는 소유 record는 후보가 밀려난 뒤까지 유지한다.

UploadMemory의 native page allocation/reuse/capacity bytes는 recorded의 전용 3개 counter에만 들어간다. 논리 uploadCalls/uploadBytes, 일반 stage bytes/operations/CPU, submitted/completed로 중복 집계하지 않는다. protocol 및 Windows pipe namespace는 v3이며 producer/consumer/GUI는 함께 다시 빌드해야 한다. DTO와 pipe v2와의 wire 호환은 제공하지 않는다. 뷰어의 현행 경로는 `src/Extend/ExecutionVisualizer`다.

RHI PrepareLiveControls는 Model outer frame과 Renderer Scene/Canvas frame의 어떤 준비·업로드·compute보다 먼저 호출한다. 같은 frame의 반복 호출은 B의 구현에서 처리한다. Renderer 파괴는 source retirement 전에 ReleaseLiveControls를 호출하며 Model은 공유 Renderer source를 해제하지 않는다.

새 native 진입점은 Renderer/Canvas의 `--optimization-only`, Model의 `Fox.glb --optimization-only [--vs-only|--compute-only]`다. GUI와 같은 typed control 적용 경로를 in-process로 호출해 OFF/ON 복원을 적용한 동일 pose의 픽셀과 업로드/자원 변화를 검사한다. 실제 GUI/pipe 입력 검증은 별도다. 기존 default native 계약과 D3D12 builtin Model compute kernel 부재 실패는 유지한다. native 실행 및 GUI 검증은 Root가 맡는다.

최종 A 공통 검사는 **7/7 PASS, 27.35s** (`build/live-a-contracts/a-optimization-final-pinned-tests.txt`), Null 묶음은 **3/3 PASS, 8.29s** (`build/live-a-model-null/a-optimization-final-pinned-tests.txt`)다. 별도 중복 없는 CTest 계약은 10개다. RHI의 PendingBounds는 종료 source의 불필요한 원본 후보를 즉시 해제한 뒤 통과했고, 추가 반복 2회도 PASS했다 (`a-optimization-rhi-repeat-tests.txt`). 읽기 전용·metadata 및 전송 테스트, 원본 pinning 테스트는 구현 전 실패를 보존했다 (`a-optimization-schema-red-tests.txt`, `a-evidence-red-tests.txt`, `a-evidence-pinning-red-tests.txt`). 신규 Null 정책 소비 테스트도 미등록 상태에서 실패를 확인했다 (`a-optimization-red-tests.txt`).

최종 계약은 기본값/actual/previousActual/owner scope, 10개 공개 정책의 적용 및 활성 조건, 정상 late 완료 event/link 보존, 2048 event·4096 link·128 GPU span 한도 및 local loss, 별도 복사 예산 부재, source retirement의 예약 반환, revision 변경 후 원본 pin 유지, v3 roundtrip/불일치 거부와 실제 Windows pipe 재연결을 포함한다. `git diff --check`, A 파일 23개의 trailing whitespace 및 기존 HEAD/stash/tests skip-worktree 39개를 확인했다. 변경 source hash와 로그 경로는 `build/live-a-contracts/a-optimization-final-provenance.json`이다. native 성능 수치나 GUI 실행 결과로 해석하지 않는다.

현재 v3 단계에서 A가 수정한 파일은 다음 23개다. 이 단계의 삭제·이동은 없으며 뷰어 폴더 이동은 C 소유다. Canvas 2개 파일의 A 초안은 B에게 이관했다.

```text
CMakeLists.txt
src/Public/dyf/Diagnostics/LiveProfiling.h
src/Diagnostics/LiveProfiling.cpp
src/Diagnostics/LiveProtocol.cpp
src/Diagnostics/LiveTransport.cpp
src/Public/dyf/RendererConfig.h
src/Public/dyf/Renderer.h
src/dyf/RendererLive.cpp
src/dyf/Renderer.cpp
src/dyf/DrawData.cpp
src/Public/dyf/Extends/Model/ModelRenderer.h
src/Extend/Model/ModelRenderer.cpp
tests/CMakeLists.txt
tests/LiveProfiling/CMakeLists.txt
tests/LiveProfilingTests.cpp
tests/LiveProtocolTests.cpp
tests/LiveAggregationTests.cpp
tests/LiveModelProfilingTests.cpp
tests/RendererPerformanceSupport.h
tests/RendererPerformanceNativeTests.cpp
tests/ModelResourceReuseNativeTests.cpp
docs/execution-visualizer/SharedContracts.md
docs/execution-visualizer/A_ImplementationAndVerification.md
```

이관된 Canvas 초안: `src/dyf/CanvasRenderer.cpp`, `tests/CanvasReuseNativeTests.cpp`. 빌드·검사 산출물은 `build/live-a-contracts`와 `build/live-a-model-null`에 생성했다. Root의 native tree와 C의 GUI 파일은 A가 수정하거나 실행하지 않았다. commit/push/reset/stash/index 변경은 없다.

## 변경과 영향

- 공개 Diagnostics DTO/생산자 API와 실제 LiveClient를 분리된 정적 라이브러리로 연결했다. `DY_ENABLE_LIVE_PROFILING`과 `DY_BUILD_EXECUTION_VISUALIZER`는 독립 옵션이며 기본값은 OFF다. producer OFF 클라이언트도 실제 ON producer에 연결한다.
- 매 프레임의 고정 요약, 모든 revision을 포함한 완료 1초 평균, 실행 전체 1초 평균의 Low/High, 고정 이전 baseline, 10초 history를 유지한다. GPU 지연 완료는 원래 device/source/frame/revision에 반영하며 list coverage의 같은 clock domain 구간만 합친다. MainForward/Shadow/Model compute는 중첩 semantic detail이다.

## 완료 1초 집계와 실행 Low/High (v2)

`recent`는 소스의 첫 BeginLiveFrame을 원점으로 하는 마지막 완료 1초 구간이다. 원본 EndLiveFrame마다 누적하므로 접속 전 프레임, 상세 예산 소진, 8192개 원본 보관 한도를 넘긴 프레임도 필수 counter/CPU 평균에 포함한다. 여러 revision이 섞인 구간은 appliedRevision=0/mixedRevision=true로 표시하며 원본 frame/revision은 유지한다. 첫 구간 완료 전과 빈 구간은 Unavailable이다.

`recentSeconds`는 최근 10초 내 완료 구간을 최대 10개 전달한다. GUI가 늦게 접속하거나 다시 접속해도 생산자가 보존한 정확한 1초 추이를 받으며, 원래 100ms/revision history를 재평균할 필요가 없다. `runRanges`의 fps/frame/cpuRender/gpuMeasured/drawCalls/instances/batches/uploadBytes/objects는 세션·소스 시작 이후 완료 1초 평균의 low/high/windows다. 같은 세션 재접속과 설정 변경으로 초기화하지 않는다.

최근 10초 구간은 늦은 GPU·counter 결과로 보정하고 매번 해당 후보의 극값을 다시 구한다. 더 오래된 유효 극값은 고정한다. GPU는 그 구간에 Pending/Unsupported/Unavailable 표본이 하나라도 있으면 Low/High 후보가 되지 않는다. 원본 보관 기한을 넘긴 미확정 GPU는 손실/부재로 남기며 0 또는 Ready를 만들지 않는다. 필수 집계 잠금은 frame -> source run 순서이며 전역 상태 잠금을 다시 잡지 않는다. 기존 control baseline/after는 revision을 구분한 1초 비교로 유지한다.

Protocol과 Windows pipe 이름은 v2다. 공개 DTO 변경으로 공통 검사, producer ON/OFF client, Model/Canvas/RHI 계약, 공개 헤더 외부 소비자, GUI 및 GUI 상태/IPC 검사는 모두 재컴파일해야 한다. A의 새 `LiveCompletedSecondAggregation`은 테스트 전용 시계로 완료 경계, 혼합 revision, 실측 0, GPU 극값 철회, TTL 이후 미확정 GPU, 접속 없이 한 구간의 9000프레임 및 상세 예산 독립성을 검증한다. 기존 코드에서 첫 완료 전 값이 노출되는 실패를 먼저 확인했다 (`build/live-a-contracts/a-seconds-red.txt`). Native/GPU 및 GUI 최종 검증은 Root/B/C 일정으로 별도 수행한다.

B 리뷰의 보정 중 할당 실패를 테스트 전용 주입으로 재현했다 (`build/live-a-contracts/a-v2-allocation-red.txt`). 해당 1초 전체를 폐기하고 11개 고정 표식으로 무효화하여 최초 map 할당 실패나 부분 보정 뒤의 정상 일부 평균·가짜 0·unsigned underflow를 막는다. 무효 구간의 recent는 frameCount=0/Unavailable, recentSeconds에는 부재, collector loss에는 누락으로 제공한다. 후속 늦은 보정은 그 구간에 다시 진입하지 않으며 다음 구간은 정상 수집한다. 기존 원본 latest/detail과 유효한 이전 runRanges는 유지한다. 공개 DTO 추가는 없다.

최종 v2 재빌드와 B 정적 재검토 뒤 다음 검사를 통과했다. 두 묶음의 새 집계 검사는 같은 계약이므로 서로 다른 검사는 총 8개다. 테스트용 GPU span은 수집기 계약 입력이며 실제 GPU 측정 자료가 아니다.

| v2 범위 | 최종 결과 | 증거 |
| --- | --- | --- |
| 공통·protocol·실제 Windows IPC | 6/6 PASS, 24.20s: 완료 1초/실패 주입 0.48s, 기존 collector 18.77s, cold lifecycle 0.10s, callback 0.02s, protocol 4.01s, 실제 재접속 0.69s | `build/live-a-contracts/a-v2-reviewed-common-tests.txt`, 같은 경로의 `a-v2-reviewed-build.txt` |
| 공식 Null 집계·Model/Canvas·RHI | 3/3 PASS, 7.06s: 집계 0.48s, Model/Canvas 0.24s, RHI 6.22s | `build/live-a-model-null/a-v2-reviewed-null-tests.txt`, 같은 경로의 `a-v2-reviewed-null-build.txt` |

이번 단계 신규 파일은 `tests/LiveAggregationTests.cpp` 1개다. 수정 파일 11개는 `src/Public/dyf/Diagnostics/LiveProfiling.h`, `src/Diagnostics/LiveInternal.h`, `src/Diagnostics/LiveProfiling.cpp`, `src/Diagnostics/LiveProtocol.cpp`, `src/Diagnostics/LiveTransport.cpp`, `tests/LiveProfilingTests.cpp`, `tests/LiveProtocolTests.cpp`, `tests/CMakeLists.txt`, `tests/LiveProfiling/CMakeLists.txt`, `docs/execution-visualizer/SharedContracts.md`, 이 보고서다. 삭제·이동은 없다. 소스 수정은 여기서 종료하며 Root/B/C가 v2 Native/GPU·GUI 실제 실행을 이어간다. `git diff --check`와 이번 12개 파일의 trailing whitespace 검사는 PASS다. HEAD/stash 및 tests skip-worktree 39개를 재확인했다.

## Model 제어와 완료 1초 비교 (추가 기능 단계)

Model owner별 `model.skinning` enum을 기존 Renderer source에 등록했다. VS=0/ComputePreSkin=1이며 실제 GPU 기록 전 frame boundary에서 처리한다. GUI의 Compute 전환은 pipeline 준비가 성공해야 기존 setter를 호출하고 actual frame/revision을 완료한다. 실패 시 기존 모드와 revision을 유지하고 Failed로 보고한다. 가용성은 장치 Compute 지원과 실제 Model shader의 stage/bytes/size/entry를 함께 검사하며 SetShaders 뒤와 Render 경계에서 다시 제공한다. 미지원 장치와 기본 kernel 부재는 서로 다른 사유의 read-only 상태로 표시하고 사용자 compute 셰이더 등록 시 복구한다. public setter의 기존 lazy pipeline 생성 시점은 유지했다. 프로그램에서 바꾼 모드도 다음 Model frame에서 revision/control 값에 반영하며 same-value/no-op은 revision을 바꾸지 않는다. 소유자 파괴 전에 등록을 해제해 대기 요청을 취소한다.

Renderer 자원 준비도 GPU 명령을 기록할 수 있어 모델 제어를 그 준비보다 먼저 처리한다. Model/custom draw에서 적용되지 않는 stock instancing/CPU frustum culling은 기존 availability로 제한하며 일반 Renderer/Canvas entry에서 복구한다. 기존 DyfModelAnim은 ModelRenderer 생성 시 제어가 자동 등록되므로 main/CMake 변경이 필요 없다. 실제 예제 실행과 GUI 조작은 Root 단계의 증거로 확인한다.

`ControlStatus.baseline/after`는 SourceRun의 원본 완료 고정 1초를 재사용한다. baseline은 실제 적용 직전 완료된 단일 old revision 구간에 고정하고, after는 appliedNs 이후 시작한 현재 revision의 최신 완료 1초로 갱신한다. 혼합/빈/손실/부족 구간은 frameCount=0으로 비교 불가다. 다른 revision으로 바뀐 이전 status의 마지막 비교 구간은 유지하며, 보관 중 늦은 결과는 같은 원본 구간만 보정한다. GPU 수치 비교는 Ready 표본이 있고 pending/unsupported/unavailable 개수가 모두 0일 때만 가능하다. 공개 DTO layout과 protocol v2는 바뀌지 않았다.

비교 상태 갱신은 100ms 스냅샷 경로와 revision 변경 직전에서 수행한다. 최대 256개 요청을 매 EndLiveFrame마다 Finish/복사하지 않는다. 원본의 필수 1초 누적은 기존대로 EndLiveFrame에서 유지한다. 추가 private 비교 cache/중복 누적 구조는 만들지 않았다.

새 계약 검사는 기존 구현에서 Model control 미등록 및 부분 baseline 노출로 먼저 실패했다 (`build/live-a-model-null/a-model-controls-red.txt`, `build/live-a-contracts/a-fixed-comparison-red.txt`). Model 검사는 VS↔Compute dispatch 0↔1, actual frame/revision, no-op, programmatic setter, pipeline 실패/회복, 미지원 장치, 다른 thread 요청 거부, owner 파괴 취소, 일반 Renderer/Canvas scope 복구를 포함한다. 고정 비교 검사는 첫 부분 구간 배제, rapid toggle 혼합 구간, 최신 after 갱신, 이전 revision 보존, 원본 GPU 지연 보정을 포함한다.

기존 ModelResourceReuseTests도 Null compatibility 대상으로 연결했다. 이 fixture는 Model 파괴 후 set/query가 0이라고 가정하므로 빌드 폴더의 wrapper에서 별도 장치-level query를 소유하는 collector를 끄고 원래 자원·실패 주입 계약을 검사한다. legacy 검사 원본과 Renderer/device query 수명은 변경하지 않았다. 새 제어/계측 검사는 collector ON으로 별도 유지한다. Native/GPU 성능이나 실제 GUI 검증으로 해석하지 않는다.

최종 공통 검사는 6/6 PASS, 29.47s (`build/live-a-contracts/a-features-verified-common-tests.txt`)다. 최종 Null 묶음은 4/4 PASS, 6.80s (`build/live-a-model-null/a-model-kernel-final-tests.txt`): 고정 집계/비교 0.49s, Model 제어·가용성 0.29s, OFF legacy wrapper 0.12s, RHI 5.72s다. 집계 검사가 두 묶음에 겹치므로 서로 다른 CTest 계약은 9개다. 마지막 빌드는 `a-features-verified-common-build.txt`와 `a-model-kernel-final-build.txt`에 기록했다. 커널 부재 가용성 회귀는 보완 전 `a-model-kernel-availability-red.txt`에서 먼저 실패했다.

Root의 D3D12 full native Model fixture는 VS animation/6 pose pixel/warm 경로를 통과한 뒤 Compute shader 생성에서 실패했다 (`build/live-root-review/features-model-d3d12-native.txt`). 실제 원인은 ModelShaderAssets.cpp가 DY_MODEL_SHADERS_COMPUTE 없을 때 빈 descriptor를 반환하고, Shaders/ShaderAssets.cmake가 Vulkan에서만 mesh_skinning_cs.glsl을 생성하는 현재 backend 범위다. 장치의 Compute 지원과 기본 Model kernel 존재는 다르다. 실패 로그는 보존하며 이를 full native PASS로 세지 않는다. 이번 단계는 기본 D3D12를 VS/read-only/정확한 사유로 표시하며 새 HLSL skinning을 포팅하지 않는다. Vulkan 실제 compute 및 GUI의 최종 증거는 Root 단계에서 추가 확인한다.

이번 기능 단계는 기존 파일 12개를 수정했다: `src/Public/dyf/Diagnostics/LiveProfiling.h`, `src/Public/dyf/Extends/Model/ModelRenderer.h`, `src/Extend/Model/ModelRenderer.cpp`, `src/Public/dyf/Renderer.h`, `src/dyf/Renderer.cpp`, `src/dyf/RendererLive.cpp`, `src/Diagnostics/LiveProfiling.cpp`, `tests/LiveAggregationTests.cpp`, `tests/LiveModelProfilingTests.cpp`, `tests/CMakeLists.txt`, `docs/execution-visualizer/SharedContracts.md`, 이 보고서다. 신규 source 파일, 삭제·이동, DyfModelAnim main/CMake 변경은 없다. legacy wrapper와 빌드/검사 로그만 build/live-a-*에 생성했다. Root의 native build tree는 A가 빌드하거나 실행하지 않았다.

## 기존 계측과 보호

- Windows QPC 절대 ns 및 submission 시점 calibration으로 CPU 배치를 계산한다. 원래 32bit 구간의 wrap 판별이 모호하면 CPU 배치를 제공하지 않는다. 제공되지 않은 calibration error는 unknown으로 유지한다.
- Renderer의 실제 설정 적용 경계에서 typed request 상태를 완료한다. superseded/no-op/failed 상태, float exposure의 실제 표현값, ThreadPool 가용성, SIMD의 빌드 시점 read-only 상태를 구분한다. Model compute와 nested Renderer는 같은 프레임·revision을 사용한다.
- Scene/Entity/Canvas/Model ID, CPU 배열/range/batch 결정, 실제 RHI resource 링크와 ThreadPool context를 연결한다. Canvas만 렌더링하면 입력 Draw 수를 gauge로 제공하고, Scene overlay는 Scene 객체 gauge를 유지한다. Canvas 제출 fence도 `GetLastRenderCompletion`으로 제공한다.
- GPU 작업·설정 변경이 없는 busy acquire 반복은 동일한 열린 프레임에 누적한다. 실제 작업을 포함한 출력 생략은 Skipped로 보존한다. 완료된 Present만 기존 owner scope의 retry 표식을 지운다. 다른 TLS의 종료 순서가 뒤바뀌어도 자기 mutex를 잠그지 않으며, 가장 최근 종료 프레임을 표시한다.
- retire/disable은 비활성 retry를 Discarded로 닫는다. cold disable은 서버를 생성하지 않으므로 collector보다 server가 먼저 파괴되는 순서를 보장한다. held snapshot은 late detail 추가, 누락, retire, disconnect 후에도 변하지 않는다.
- 공통 상세 예산 32MiB에 deferred vector capacity도 포함한다. 선택적 event/link는 24MiB에서 제한해 제출 계수와 GPU 결과에 공간을 남긴다. 고정 deferred 요약은 모든 명령을 포함하며 상세만 누락 표시한다. 프레임 상한 8192, 명령 generation 상한 4096, batch 상한 65536 및 비사용 batch 10초 만료를 적용한다.
- same user/logon SID/session, peer PID, remote client 차단, 제한된 ACL, SQOS identification을 적용한 로컬 Named Pipe를 사용한다. packet/decoded allocation 예산은 64MiB이며 길이·enum·variant·finite 값·GPU 구간을 검증한다. 느린 reader를 기다리는 작업은 worker에서 처리한다.

## 이전 v1 검증

빌드는 MSVC 19.44 / VS 2022 x64이며 기존 glfw/stb/fastgltf/ufbx 캐시를 사용했다. Debug 계약 검사와 Release native 소비자를 구분한다.

| 범위 | 현재 결과 | 증거 |
| --- | --- | --- |
| Collector | 최종 PASS 18.65s: 10,000 retry 1프레임, 100,000 deferred draw 계수, 누락 cap, out-of-order 종료, 지연 GPU/revision/구간 합집합, 10초 수명, disable/enable | `build/live-root-review/final-integrated-test.txt` |
| Cold lifecycle | 최종 PASS 0.13s: cold disable→enable→register, 실제 self-client 접속, worker 종료 | 같은 로그 |
| Protocol | 최종 PASS 8.13s: roundtrip, malformed 형식, decoded 예산, late GPU 구간, wrap/error/frequency calibration | 같은 로그 |
| 실제 IPC | 최종 PASS 1.05s: 새 producer-OFF client 프로세스 2개가 반대 값을 실제 적용, request ID 구분, held snapshot, 느린 reader에도 producer 진행 | 같은 로그; A의 기존 256frames/10.028ms 관찰은 느린 reader에서도 producer가 진행하는지 검사한 결과로, native 비용 측정과 구분 |
| 공식 Null RHI | 최종 PASS 9.19s: B의 fence/deferred/retry/native-boundary 계약을 공식 CMake 정의로 실행 | 같은 로그 |
| Model/Canvas | 최종 PASS 0.48s: 같은 epoch compute/Main, shader-only 설정 보류, 실패 20회 후 query pool 회복, Canvas split/gauge/move, 5,000 Canvas/1,000 Scene acquire retry, 실제 compute Skipped 보존, typed controls 실제 적용 | 같은 로그 |
| Callback 수명 | 최종 PASS 0.05s: 자기·ancestor 해제와 다른 thread 대기 보호 | `build/live-root-review/final-callback-test.txt` |
| Release native RHI | D3D12 PASS 7.76s, Vulkan PASS 8.50s, 각 1/1 | `build/live-root-review/final-{d3d12,vulkan}-native-test.txt` |
| Viewer 계약 | 최초 process/state/native driver 3/3 PASS; GLFW 수정 후 영향 범위 4/4 PASS(0.21s), window ownership 회귀 추가 | `build/live-root-review/final-viewer-contracts.txt`, `glfw-fixed-viewer-contracts.txt` |
| Release GUI 실제 입력 | D3D12 20회 적용/표시 p95 185.869ms, Vulkan 4회 기능·보조 지연 확인; 두 GUI exit0 | 아래 GUI 절의 원본 보고서·exit 기록 |
| 외부 D3D12 소비자 | GLFW 수정 후 Root 재검증 PASS 3/3, 11.94s | `build/live-root-review/external-d3d12-glfw-{source,config,build,test}.txt` |
| 외부 Vulkan 소비자 | GLFW 수정 후 Root 재검증 PASS 3/3, 15.01s | `build/live-root-review/external-vulkan-glfw-{source,config,build,test}.txt` |
| OFF 호환성 | producer OFF 실제 client IPC, Root Null OFF 계약/RenderGraph 및 D3D12 OFF 예제14/15 빌드 확인 | `build/live-rhi-null-off/Testing/Temporary/LastTest.log`, `build/live-root-review/build-d3d12-off-final.txt` |
| 소스 확인 | `git diff --check` PASS. BackendBoundary: 55 files / 기존 18 failures 유지 | `tests/BackendBoundaryTests.py --repo .` |

최신 외부 소비자는 정상 Windows의 저장소 밖 개인 TEMP에서 공개 헤더만 사용하는 2파일 source와 `DY_BUILD_EXAMPLES=OFF` 독립 superproject로 구성·컴파일·실행했다. GLFW 수정 후 raw/Renderer/Canvas 6개를 별도 재검증했으며, 기존 `external-*-final-*` 로그는 이력으로 유지한다. 아래 hash는 앞선 source 검증 기록이다.

```text
main.cpp       B6BF3B6A2F605835B64F1E735B8DBD73339CECA767DF19E54C4DAEB91A659633
CMakeLists.txt AE4507B8136C4B4E8194278B6D83680F2C63C5A7AF341E29D94A9490AB1A3B92
```

Raw는 1024→256 byte 업로드와 사용자 gauge/control을 검증한다. Renderer는 16단계에서 instancing 1→256 DrawIndexed, no-op revision, 0.1 exposure의 실제 float, dirty range 33개→full 16384 byte, 실행 중 객체 256→257 및 full 16448 byte, culling 1개, lighting/shadows/quality, HDR의 추가 ToneMap Draw, VSync와 parallel 적용을 검증한다. Canvas는 5입력→4 Draw, texture/scissor/viewport split, 안정 batch ID를 검증한다.

Model fixture는 native 경계만 inert인 CPU/Null 계약이다. opaque 테스트 셰이더를 사용하며 stock shader 또는 실제 GPU 실행의 증거로 해석하지 않는다. Null에는 stock Renderer/Canvas 셰이더가 없으므로 외부 소비자는 해당 경로를 skip77로 명시한다. 20회의 의도적 제출 실패 뒤 query 회복을 검증한 후, 예약된 Model shader 복원의 실제 Canvas 업로드/제출을 먼저 실행하여 빈 acquire retry 검사의 준비 상태를 확정했다.

Root가 실행 파일을 재빌드한 뒤 공식 공통·IPC·Model·RHI 6/6(38.14s), 별도 callback 1/1, Release native 두 backend 각 1/1, Viewer 계약 3/3을 재검증했다. 이 기존 12개가 모두 PASS했고, 후속 GLFW window ownership 회귀 1개가 추가됐다. 이번에는 총 13개 계약 중 Viewer 4/4와 별도의 외부 소비자 6개를 재실행해 PASS했으며, 모든 13개 계약을 한 번에 재실행한 것은 아니다. D3D12/Vulkan의 example14/15 네 바이너리도 현재 GLFW patch 반영 빌드 PASS다. 이전 `integrated-null-final-test.txt`의 Model warm-up 보완 전 5/6 결과는 위 최종 로그로 대체한다. 기존 `P2ModelRendererTests`의 `#define private public` MSVC private/public symbol LNK2019는 별도 기존 테스트 한계이며, 제품 접근성을 public으로 바꾸지 않았다.

Root가 검증한 D3D12/Vulkan raw14/15 real IPC 900프레임 결과는 각각 PASS이며 frame/GPU span loss는 0이다. 선택적 event 누락은 별도 loss 필드에 남는다. 이 결과와 CPU/Null 검사는 GUI 사용자 조작이나 표시 p95 250ms 목표의 검증을 대신하지 않는다. 비용 실측·비교는 별도 성능 절에 기록했다.

## 최종 callback 수명 보완

`PollControls`의 실행 mutex를 보유한 callback이 자신의 `ControlRegistration`을 해제하면, 이전 `Unregister`는 같은 non-recursive mutex를 다시 잠갔다. 현재 MSVC에서는 정리가 전체 PollControls 종료까지 지연되어 다음 callback 전 해제 검사가 FAIL했고, 다른 표준 라이브러리에서 안전한 재진입이라고 보장할 수 없다.

같은 thread의 실행 중 callback/ancestor를 작은 TLS invocation chain으로 확인한다. 등록은 즉시 제거하고 요청을 Cancelled로 만들며, handler 캡처는 해당 callback 반환 후에 정리한다. 다른 thread의 해제는 기존 invocation mutex 대기를 유지한다. Public API/DTO/backend/GUI 코드는 바꾸지 않았다.

`LiveControlCallbackLifetime` 검사는 자기 move-assignment 해제 후에도 capture에 접근 가능함, 다음 callback 전에 capture 해제, nested callback의 ancestor 해제, 다른 thread의 해제가 callback 반환 전 완료되지 않음을 검증한다. Red/Green 증거는 `build/live-a-contracts/a-callback-{red,green}-test.log`이며 공식/독립 CMake에 검사명을 연결했다.

A 재검증은 공통 5/5 PASS(27.74s: collector 18.18s, cold lifecycle 0.12s, callback lifetime 0.03s, protocol 8.25s, IPC 0.99s), Model/Canvas PASS 0.61s다. 증거는 `build/live-a-contracts/a-callback-final-tests.log`, `build/live-a-model-null/a-callback-followup-test.log`다. 그 뒤 Root의 위 최종 재빌드·12개 재검증도 PASS했다. 이 보완에서 수정한 파일은 `src/Diagnostics/LiveProfiling.cpp`, `tests/LiveProfilingTests.cpp`, `tests/CMakeLists.txt`, `tests/LiveProfiling/CMakeLists.txt`, 이 문서와 `SharedContracts.md` 6개이며 신규/삭제/이동 파일은 없다. A가 추가 Native 기능·성능 실행을 수행한 것은 아니다.

## Root 성능 측정 18회

원시값과 실행 파일 hash: `build/live-root-review/performance-summary.json`. 고정 조건은 Release, 800×600 FIFO, 256 Draw/frame, 900 frames, validation/Tracy/PIX OFF다. 각 상태 3회, 실행 순서를 회전했고 아래는 중앙값이다. 이 18회는 callback 수명 보완 및 GLFW 수정 전 바이너리의 역사적 비교 결과다. 측정 중 제어 요청을 보내지 않아 자기 해제 보완 경로는 실행하지 않았다. 최신 GUI 빌드의 재측정으로 해석하지 않는다.

| Backend | OFF Record ms | ON 미연결 Record ms | ON 연결 Record ms | 전체 target CPU: 연결 대비 OFF 변화 |
| --- | ---: | ---: | ---: | ---: |
| D3D12 | 0.089531 | 0.557094 | 0.647393 | +4.72% |
| Vulkan | 0.104953 | 0.609206 | 0.632151 | +6.89% |

측정 target SHA256은 D3D12 OFF `33D3062A4AABC0A326C7C58099C0CDBE499459619C4E4F8650F704A08EB7527A`, ON `CC07015E392B64A6B9E96D5B8D4C7B605908D2642689D80979BB4F023D3B2D39`, Vulkan OFF `A7AB465FBE88BFF66BBC81F93ED2E2038E8E1463017F380E9CB7F3EE247E599D`, ON `91E196533BEB7F6169820E59CA0F3763EC97FD63980B8291CDDBCA733EEBB36F`다.

전체 프로세스 CPU는 초기화·busy acquire·종료를 포함하며, FIFO cadence는 throughput 비용을 가릴 수 있다. Record 시간은 GPU 실행·Present·capture 시간을 포함하지 않는다. 관찰 driver의 CPU는 GUI 자체 비용이 아니다. 이 18회 당시에는 기존 Debug viewer PID39236이 배경 실행 중이었고 Release GUI 자체 측정은 아직 없었다. 후속 최신 GUI의 별도 측정은 다음 절과 구분한다.

## GLFW 수정 후 실제 GUI 검증과 자체 비용

C/Root가 확인한 GLFW의 외부 프로세스 창 포인터 접근을 C 소유 dependency patch와 window ownership 회귀로 수정했다. 최신 Release GUI SHA256은 `DB42B26E316B477AEA69FEDC80A946598695C1E522EEB78422AE05227A188DD2`다. Root가 Sky의 실제 checkbox/Apply 입력과 screenshot으로 검증했으며 API 제어 요청으로 대체하지 않았다. D3D12 example14 및 Vulkan example15 빌드와 수정 영향 범위 Viewer 4/4 PASS는 `build/live-root-review/glfw-fixed-{d3d12,vulkan}-build-serial.txt`, `glfw-fixed-viewer-contracts.txt`에 기록됐다.

| 실제 GUI 검증 | 결과와 범위 |
| --- | --- |
| D3D12 example14 | Instancing 20회, Applied revision 2~21, Draw/batch 1↔256, objects 256. 20표본 p95 **185.869ms**로 표시 목표 250ms를 충족했다. min 65.418 / max **316.368** / mean 142.33615ms이며 1회는 250ms 초과다. 모든 전환이 250ms 이하였다거나 다른 환경에도 보장된다고 해석하지 않는다. GUI 10,791 Present, QPC 일치, exit0. |
| D3D12 상호작용 | 대상 창 전면 전환·최대화·복원·뷰어 복귀 PASS. 연결 해제 시 age 136→6711ms 증가, 재연결 후 59ms 복귀 PASS. Expand IDs→Batch23 선택에서 elements 256 / bytes 8192 / EndOfInput / 원래 frame 표시 PASS. |
| Vulkan example15 | create↔reuse 4회, Applied revision 2~5, upload 8192bytes/frame 및 buffer ID 재사용/변경 확인. 실제 GPU tab의 Vulkan 1queue, 원래 frame의 Completed 0.015ms, calibration error 2816ns 표시 확인. 대상 창 최대화·복원·뷰어 복귀 PASS. 4표본 min 111.112 / max 142.252 / mean 130.27125ms, 계산 p95 142.252ms. 4개는 기능·보조 지연 표본이며 통계적으로 충분한 p95 일반 보장이 아니다. GUI 50,140 Present, QPC 일치, exit0. |

원본은 `build/live-root-review/gui-fixed-d3d12-example14.txt`, `gui-fixed-vulkan-example15.txt`, 정상 종료는 `gui-fixed-{d3d12,vulkan}-exit.txt`다. Root의 독립 재계산 `gui-latency-summary-final.json`도 일치한다. 표시 지연은 producer의 실제 적용부터 GUI Present까지이며 모니터 scan-out 측정은 아니다. 소유 검증 프로세스는 정상 종료했다.

최신 GUI 자체 비용 원본은 `build/live-root-review/gui-fixed-cost-d3d12-instanced.json`이다. GUI 1540×960, D3D12 example14 instanced 800×600 FIFO, 연결된 기본 그룹 Flow에서 41.8651039초를 측정했다. CPU 6.78125초는 논리 CPU 1개 기준 16.1978578%, 22개 논리 CPU로 환산하면 약 0.736%다. working set 177.836MiB / private 197.543MiB다. 사용자 배경 게임은 그대로 둔 조건이다.

| GUI GPU counter — 각 15표본 | mean | max |
| --- | ---: | ---: |
| 3D engine | 0.4634456% | 0.5112983% |
| copy engine | 1.3614387% | 1.53435% |
| graphics_1 engine | 0.8489737% | 1.0215835% |

이는 프로세스의 개별 GPU 엔진 counter이며 합계를 전체 GPU busy로 해석하지 않는다. 해당 GUI 크기·대상·배경 부하·짧은 측정 구간의 관찰값이며, 모든 부하나 장시간 안정성의 일반 보장은 아니다. 이번 A 변경은 이 문서와 `SharedContracts.md`의 증거 갱신뿐이며 제품 코드와 GUI README는 수정하지 않았다.

## 생성 파일 18개

```text
src/Public/dyf/Diagnostics/LiveProfiling.h
src/Diagnostics/LiveInternal.h
src/Diagnostics/LiveGpuTiming.h
src/Diagnostics/LiveProfiling.cpp
src/Diagnostics/LiveProtocol.cpp
src/Diagnostics/LiveTransport.cpp
src/dyf/RendererLive.cpp
tests/LiveProfilingTests.cpp
tests/LiveAggregationTests.cpp
tests/LiveProtocolTests.cpp
tests/LiveTransportTests.cpp
tests/LiveTransportClient.cpp
tests/LiveModelProfilingTests.cpp
tests/LiveProfiling/CMakeLists.txt
tests/Fixtures/LiveConsumer/CMakeLists.txt
tests/Fixtures/LiveConsumer/main.cpp
docs/execution-visualizer/SharedContracts.md
docs/execution-visualizer/A_ImplementationAndVerification.md
```

## 수정 파일 16개

```text
CMakeLists.txt
cmake/Engine_Options.cmake
src/Core/ThreadPool.cpp
src/Extend/Model/ModelRenderer.cpp
src/Extend/Model/ModelScene.cpp
src/Public/dyf/Canvas.h
src/Public/dyf/Extends/Model/ModelRenderer.h
src/Public/dyf/Renderer.h
src/Public/dyf/Scene.h
src/dyf/Canvas.cpp
src/dyf/CanvasRenderer.cpp
src/dyf/DrawData.cpp
src/dyf/Renderer.cpp
src/dyf/Scene.cpp
tests/CMakeLists.txt
tests/BackendBoundaryTests.py
```

삭제·이동 파일은 없다. 컴파일 산출물과 A 검사 로그는 `build/live-a-*`에 생성했다. B/C의 RHI/backend/예제/GUI 변경은 이 목록에 포함하지 않았다. HEAD `050f418f83f0654f5345dddb6da61519c7546e5a`, 보존 stash `425fa74b02707b8f2bfb0911b722b3491da444cd`, 기존 tests skip-worktree 39개를 유지했으며 commit/push/reset/stash 변경은 없다.
