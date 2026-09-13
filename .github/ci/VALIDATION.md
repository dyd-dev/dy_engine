# 로컬 검증 기록

2026-09-13, 브랜치 `CI`, 기준 HEAD `aaf323657f61ac4334cfa45b6a7a1cee6bb5017b`. 아래 실행은 미커밋 작업 폴더를 명시한 `run`으로 수행했다. 현재 저장소를 stage/commit/push하지 않았으며 원격 Actions 실행이나 runner 등록을 완료했다는 뜻은 아니다.

## 자동 선택 추가 검증

기준 커밋 `8707de5` 이후 CI에 자동 선택을 추가했다. 제품·예제 구현은 변경하지 않았다.

- Windows 검사 28개 통과(대화형 GUI 검사 1개는 기본 생략). 별도 임시 Git 저장소에서 문서만 바꾼 실제 push는 빌드 없이 통과하고 예제 소스 변경 push는 검사 실패 시 전송을 차단함을 확인했다.
- Linux에서도 최종 검사 28개 통과(대화형 GUI 검사 1개는 기본 생략). actionlint로 workflow 형식을 검증했다.
- 미커밋 선택 규칙을 검사 생략으로 바꿔도, 전송할 커밋의 선택 규칙을 사용하여 우회되지 않음을 확인했다.
- 이름 변경의 이전·새 경로, 새 브랜치/비교 기준 없음, 백엔드별 선택, 공유 자산·CPU 검사 연결, 불명확한 입력의 전체 검사 전환을 검증했다.
- 실제 임시 CMake 프로젝트에서 연결한 오브젝트 라이브러리와 자산 원본을 수집했다. 다른 예제에 문법 오류를 넣어도 선택한 타깃만 빌드되어 성공했다.
- 실제 저장소에서는 Cube 소스 변경을 모사한 커밋 ID/경로 입력으로 Cube만 선택했다. Linux Xvfb 소프트웨어 Vulkan에서 Cube만 빌드·실행하여 PASS, 제품 소스·셰이더 141개 무변경을 확인했다. 이는 실제 저장소를 push했다는 뜻은 아니다.
- Windows에서도 Cube만 선택·빌드했지만 창의 전면 상태가 확보되지 않아 실행 관찰은 BLOCKED였다. 이를 PASS로 처리하지 않았다.

선택 결과: `build-ci/external-linux/ci-logs/selection.json`. 기존 D3D12 링크 오류와 Metal 관찰 미구현은 수정하지 않았다. 원격 Actions 실행은 아직 수행하지 않았다.

자동 선택 작업의 추가 파일은 `selection.py`, `test_selection.py`다. 수정 파일은 `.github/ci/ci.py`, `.github/ci/test_ci.py`, `.github/ci/CMakeLists.txt`, `cmake/ExampleSupport.cmake`, `.github/workflows/ci.yml`, 루트와 CI의 `README.md`, 이 검증 기록이다. 예제 CMake와 src/에는 변경이 없다.

## 백엔드 복구 후 현재 상태

사용자 요청으로 D3D12Device.cpp, MetalDevice.mm, VulkanDevice.cpp, IDevice.cpp, Window.h를 HEAD로 복구하고 파일 내용을 비교했다. 추가 요청에 따라 src/Backends 폴더 전체를 HEAD로 복구하여 D3D12·Metal·Null·Vulkan의 CMake 변경과 줄바꿈 차이까지 제거했다. 백엔드 폴더 밖의 CI 구성은 남아 있다. 아래 빌드·실행 결과는 복구 이전 기록이며 현재 백엔드의 통과 증거가 아니다. 복구 후 재검증 결과는 다음 절에 기록한다. 독립 GPU 검사에 자원 카운터 의존이 남아 있으므로 D3D12/Metal에서는 해당 판정이 실패할 수 있다.

## 백엔드 전체 복구 후 재검증

사용자 요청으로 Windows에서 현재 작업 폴더를 다시 빌드·검사했다. 제품 코드와 CI 설정은 수정하지 않았다.

| 대상 | 결과 |
|---|---|
| Vulkan Debug 빌드 | PASS |
| Vulkan 원본 예제 실행 | 11개 경우 중 9 PASS, ShadowCube·LightingLab 2 FAIL(장면 영역이 배경만 보임) |
| Vulkan 독립 GPU 검사 | clear·triangle·depth·texture 4 PASS |
| D3D12 Debug 빌드 | FAIL: D3D12CreateDevice·CreateDXGIFactory1 등의 LNK2019/LNK1120 |
| D3D12 실행 검사 | 빌드 실패로 실행하지 못함. 기존 바이너리로 대신 검사하지 않음 |
| Metal | Windows 환경이므로 실행하지 않음 |
| 제품 소스 무변경 | 두 실행 모두 소스·셰이더 141개 해시 일치, src/ Git 변경 없음 |

D3D12는 생성된 Cube.vcxproj의 AdditionalDependencies에 d3d12/dxgi 라이브러리가 없음을 확인했다. 원래대로 복구한 백엔드 CMake의 오브젝트 포함 방식과, 백엔드 직접 연결을 제거한 현재 예제 CMake 사이의 연결 불일치다. 이는 현재 CI/예제 빌드 구성 문제이며 백엔드 런타임 결함으로 판정하지 않는다. 수정하지 않고 실패를 기록했다.

최신 결과는 `build-ci/external-vulkan/ci-logs`와 `build-ci/external-d3d12/ci-logs`에 있다. 아래 복구 이전 표의 D3D12 실행 결과는 이번 결과와 구분해야 한다.

## 복구 이전 검증 결과

| 검사 | 실제 결과 | 증거 |
|---|---|---|
| CI 자체 검사 | 최종 20개 PASS. Linux Xvfb 실제 창 검사 포함 | unittest 전체 실행, 7.265초 |
| Windows 실제 관찰기 | RGB 캡처·warmup·정상 닫기·충돌·시간 초과·콘솔 종료 PASS | test_observe.py, 2개 6.417초 |
| Windows CPU | 옵션·애니메이션·모델 로더·RenderGraph 4영역 + 원본 RenderGraph PASS, Clang 21 ASan | `build-ci/external-cpu/ci-logs` |
| Linux CPU | 같은 4영역 + 원본 RenderGraph PASS, Clang 18 ASan/UBSan | `build-ci/external-linux-cpu/ci-logs` |
| Windows Vulkan Debug | 빌드 PASS, 예제 실행 9 PASS / 2 FAIL, 독립 GPU 검사 4 PASS | `build-ci/external-vulkan/ci-logs` |
| Linux 소프트웨어 Vulkan Debug | 빌드 PASS, 예제 실행 9 PASS / 2 FAIL, 독립 GPU 검사 4 PASS | `build-ci/external-linux/ci-logs` |
| Windows D3D12 Debug | 빌드 PASS, 예제 실행 2 PASS / 8 FAIL, 독립 GPU 검사 4 PASS | `build-ci/external-d3d12/ci-logs` |
| 소비자 프로젝트 | 기본 설정 및 부모 CI 옵션 ON에서 실제 컴파일·링크·Scene 실행 PASS | `build-ci/external-consumer/{default,inherited-ci}/ci-logs` |
| 제품 소스 무변경 | 각 실제 검사 전후 소스·셰이더 141개 해시 일치 | 각 result.json의 product-source-integrity |
| 원본 예제 복구 | 추적된 예제 코드·셰이더 53개 HEAD와 LF 정규화 기준 일치 | `build-ci/external-maintainer/source-integrity.json` |
| workflow | actionlint PASS, push-only 연결 확인 | `.github/workflows/ci.yml` |
| Metal/macOS | 실제 빌드·GPU 실행 미검증, 외부 관찰 미구현으로 runtime BLOCKED | 관찰기가 지원 불가 사유를 반환 |

예제 실행 수에는 RenderPath의 기존 CLI 3가지 경우가 포함된다. LightingLab은 CMake 선언상 D3D12 미지원이다. Python 기본 발견은 GUI 검사를 건너뛰지만 최종 Linux 검증에서는 `DY_CI_OBSERVER_LIVE=1`로 실제 수행했다. 최초 Windows 일반 샌드박스 실행은 임시 디렉터리 접근 오류가 있었고 실제 실행 환경에서 재검증했다.

최종 구성의 Release 전체/Metal 빌드를 새로 실행했다고 주장하지 않는다. workflow는 해당 빌드 행렬을 포함하지만 로컬에서 확인한 최종 제품 빌드는 위 Debug 구성과 CPU/소비자 구성이다.

## 검출한 실제 실패

- Vulkan ShadowCube·LightingLab: 초기화 대기 후에도 두 화면의 장면 영역이 배경만 보여 장면 존재 검사 FAIL. Windows 물리 GPU와 Linux 소프트웨어 GPU에서 모두 검출했다.
- D3D12 HelloRenderer·Cube·TexturedCube·ShadowCube·RenderPath batched/bindless: GPU가 참조 중인 자원 또는 pipeline/root signature를 먼저 해제하는 severity 1, ID 921 진단으로 FAIL.
- D3D12 LoadModel·RenderPath per-draw: 외부 정상 닫기 요청 후 종료 코드 2173으로 FAIL. 관찰기 강제 종료로 만든 실패는 아니다.

검출 결과를 통과시키기 위해 원본 예제를 수정하거나 제외하지 않았다. 이 버전의 전체 CI는 위 실패와 Metal 검증 공백 때문에 통과 상태가 아니다. 수정 후 같은 실행으로 재검증할 수 있다.

## 검사 자체에서 발견해 바로잡은 문제

D3D12 텍스처 fixture의 최초 실패는 제품 결함이 아니라 검사 앱이 공개 디스크립터 등록 계약을 빠뜨린 문제였다. 기존 공개 AllocateDescriptorSlot/UpdateDescriptorSlot을 사용하도록 보완한 후 4가지 GPU 검사가 각 2장 모두 지정 RGB와 일치했다. 최초 실패와 수정 후 결과를 `build-ci/external-d3d12/probe-validation*`에 보관했다.

초기 원본 RenderPath 관찰에서 첫 Present 이전의 흰 창을 잡을 수 있었다. 예제 소스 수정 없이 외부 warmup을 두었고 최종 Windows Vulkan의 RenderPath 3개 경우는 모두 통과했다. 원본 프로그램의 시간이나 벤치마크는 바꾸지 않았다. D3D12의 위 실패는 흰 화면 판정이 아니라 진단/종료 코드에 의한 실패다.

검사기는 의도적으로 잘못된 RGB, 빈 장면, 캡처 0/1장, 강제 종료, 조기 종료, 창 접근 불가, 소스 변경, 잘못된 콜백 순서를 넣었을 때 성공하지 않음을 확인했다. 별도 임시 Git 저장소에서 commit은 검사를 실행하지 않고 실패한 pre-push는 전송을 차단하며 기존 hook의 입력을 보존함도 확인했다. 제품 소스에 고의 결함을 남기지 않았다.

## 변경 영향

예제의 C++/헤더/셰이더는 원본으로 복구했다. 수정된 예제 파일은 빌드 등록용 CMake다. CI 전용 소스 계측·숨겨진 토글·backbuffer 캡처 주입·AST/호출 추적을 제거하고, 독립 CPU/GPU 실행기와 외부 창 관찰기로 대체했다. 이전 중앙 지원 표와 강제 종료 smoke 스크립트도 새 실행기로 대체했다.

이전에 유지했던 제품 안정성 변경 5파일은 사용자 요청으로 전부 복구했다. 제품 구현 오류를 고치는 변경은 CI 변경에 포함하지 않는다.

아래는 HEAD 대비 실질 내용 변경과 새 파일 목록이다. 줄바꿈만 달라 Git 상태에 나타나는 복구 파일은 제외한다. `.github/ci`에서 이번 전환으로 삭제한 이전 미추적 구현은 Git 삭제 목록에 나오지 않으므로 별도로 기록한다: ExampleChecks.h, contracts.py, architecture.py, instrument.py, CallTrace.cpp, 관련 기존 테스트/fixtures. 생성한 빌드·캡처·임시 검증 파일은 무시된 `build-ci` 아래에 보관했다.

| 상태 | 파일 |
|---|---|
| 삭제 | `.github/ci-support.json` |
| 삭제 | `.github/scripts/check-ci-support.py` |
| 삭제 | `.github/scripts/check-vulkan-smoke.py` |
| 생성 | [.github/actionlint.yaml](../../.github/actionlint.yaml) |
| 생성 | [.github/actions/setup-ci/action.yml](../../.github/actions/setup-ci/action.yml) |
| 생성 | [.github/ci/CMakeLists.txt](../../.github/ci/CMakeLists.txt) |
| 생성 | [.github/ci/CpuChecks.cpp](../../.github/ci/CpuChecks.cpp) |
| 생성 | [.github/ci/README.md](../../.github/ci/README.md) |
| 생성 | [.github/ci/RenderProbe.cpp](../../.github/ci/RenderProbe.cpp) |
| 생성 | [.github/ci/RhiSmoke.cpp](../../.github/ci/RhiSmoke.cpp) |
| 생성 | [.github/ci/VALIDATION.md](../../.github/ci/VALIDATION.md) |
| 생성 | [.github/ci/checks.py](../../.github/ci/checks.py) |
| 생성 | [.github/ci/ci.py](../../.github/ci/ci.py) |
| 생성 | [.github/ci/examples.json](../../.github/ci/examples.json) |
| 생성 | [.github/ci/observe.py](../../.github/ci/observe.py) |
| 생성 | [.github/ci/shaders/probe_ps.glsl](../../.github/ci/shaders/probe_ps.glsl) |
| 생성 | [.github/ci/shaders/probe_ps.hlsl](../../.github/ci/shaders/probe_ps.hlsl) |
| 생성 | [.github/ci/shaders/probe_ps.metal](../../.github/ci/shaders/probe_ps.metal) |
| 생성 | [.github/ci/shaders/probe_vs.glsl](../../.github/ci/shaders/probe_vs.glsl) |
| 생성 | [.github/ci/shaders/probe_vs.hlsl](../../.github/ci/shaders/probe_vs.hlsl) |
| 생성 | [.github/ci/shaders/probe_vs.metal](../../.github/ci/shaders/probe_vs.metal) |
| 생성 | [.github/ci/test_ci.py](../../.github/ci/test_ci.py) |
| 생성 | [.github/ci/test_consumer.py](../../.github/ci/test_consumer.py) |
| 생성 | [.github/ci/test_discovery.py](../../.github/ci/test_discovery.py) |
| 생성 | [.github/ci/test_external_contracts.py](../../.github/ci/test_external_contracts.py) |
| 생성 | [.github/ci/test_observe.py](../../.github/ci/test_observe.py) |
| 생성 | [cmake/CiChecks.cmake](../../cmake/CiChecks.cmake) |
| 생성 | [cmake/SyncAssets.cmake](../../cmake/SyncAssets.cmake) |
| 수정 | [.github/workflows/ci.yml](../../.github/workflows/ci.yml) |
| 수정 | [CMakeLists.txt](../../CMakeLists.txt) |
| 수정 | [README.md](../../README.md) |
| 수정 | [cmake/Engine_Options.cmake](../../cmake/Engine_Options.cmake) |
| 수정 | [cmake/ExampleSupport.cmake](../../cmake/ExampleSupport.cmake) |
| 삭제 | `docs/CI_Maintenance_Guide.md` |
| 수정 | [examples/01_HelloWindow/CMakeLists.txt](../../examples/01_HelloWindow/CMakeLists.txt) |
| 수정 | [examples/02_HelloRenderer/CMakeLists.txt](../../examples/02_HelloRenderer/CMakeLists.txt) |
| 수정 | [examples/03_Cube/CMakeLists.txt](../../examples/03_Cube/CMakeLists.txt) |
| 수정 | [examples/04_TexturedCube/CMakeLists.txt](../../examples/04_TexturedCube/CMakeLists.txt) |
| 수정 | [examples/05_LoadModel/CMakeLists.txt](../../examples/05_LoadModel/CMakeLists.txt) |
| 수정 | [examples/06_ShadowCube/CMakeLists.txt](../../examples/06_ShadowCube/CMakeLists.txt) |
| 수정 | [examples/07_RenderPath/CMakeLists.txt](../../examples/07_RenderPath/CMakeLists.txt) |
| 수정 | [examples/08_LightingLab/CMakeLists.txt](../../examples/08_LightingLab/CMakeLists.txt) |
| 수정 | [examples/CMakeLists.txt](../../examples/CMakeLists.txt) |
| 수정 | [examples/RenderGraph/CMakeLists.txt](../../examples/RenderGraph/CMakeLists.txt) |
