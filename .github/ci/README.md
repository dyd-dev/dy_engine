# 프레임워크 개발자용 push CI

공식 예제는 배포하는 원본 소스 그대로 실행합니다. 검사 코드는 `.github/ci`에 두고 예제의 창·출력과 별도 공개 API 테스트의 결과를 확인합니다.

## 자동 실행 범위

- 로컬에서는 push를 막는 CI hook을 사용하지 않습니다. 작업 브랜치의 push는 먼저 완료됩니다.
- GitHub Actions: 모든 브랜치의 `push`에서 이미 전송된 커밋을 검사합니다.
- 커밋·저장·스테이징·PR·예약 실행에는 연결하지 않습니다.
- 소비자 프로젝트에는 공식 예제와 검사 실행기를 자동 등록하지 않습니다. 라이브러리 빌드가 hook을 설치하지 않습니다.
- CI가 코드를 자동 수정하거나 결과 화면을 정답으로 자동 승인하지 않습니다.

## push 변경에 따른 자동 선택

push 전 원격 커밋과 전송할 커밋의 최종 차이로 선택합니다. 개발자가 검사 대상 목록을 지정하지 않습니다.
자동 검사는 GitHub Actions에서 실행합니다. 기존 로컬 검사 명령은 수동 진단용으로 남아 있지만 hook으로 연결하지 않습니다.

| 변경 | 자동 선택 |
|---|---|
| 예제의 전용 소스·자산 | 해당 예제와 그 입력을 공유하는 타깃 |
| `.glsl` / `.hlsl` / `.metal` 셰이더만 변경 | 해당 백엔드에서 관련 예제 |
| 특정 백엔드 폴더만 변경 | 해당 백엔드의 예제·독립 GPU 검사 (Null은 CPU 검사) |
| RHI·Renderer 등 공통 코드, 헤더·CMake·알 수 없는 설정 | 전체 검사 |
| 알려진 README·docs 문서만 변경 또는 최종 차이 없음 | 제품 빌드·실행 생략 |
| 새 원격 브랜치 등 비교 기준을 구하지 못함 | 전체 검사 |

CMake에서 실제 타깃 소스, 연결한 라이브러리(오브젝트 라이브러리 포함), 셰이더 폴더, 자산 배치 원본을 수집합니다.
관련 타깃만 `cmake --build --target ...`으로 빌드하고 실행합니다. 삭제된 파일도 비교에 포함하며 이름 변경은 이전·새 경로를 모두 봅니다.
헤더의 간접 include는 완전한 소스 분석을 하지 않으므로 전체 검사로 처리합니다. 알 수 없는 CMake 표현식이나 매핑되지 않는 입력도 전체 검사로 확장합니다.
CPU 검사는 실행기 단위로 선택하고, 예제 전용 변경에는 관계없는 독립 GPU 검사까지 실행하지 않습니다.

GitHub에서는 변경에 해당하는 백엔드 작업만 등록합니다. 예제 변경의 정확한 타깃·지원 범위는 각 환경에서 CMake 설정 후 결정하므로,
대상이 없는 작업에도 도구 준비·CMake 설정 비용은 들 수 있습니다. 문서 변경에도 변경 판정과 CI 자체 검사 작업은 실행하지만 제품 검사는 생략합니다.
선택하지 않은 항목은 `SKIPPED`로 기록하며 실제 기능 검증 통과로 표시하지 않습니다.

`ci-logs/selection.json`에 선택 근거와 타깃 목록을 남깁니다. `--changes-file`은 Actions가 생성한 커밋 ID와 변경 경로를 전달하는 내부 입력이며,
커밋 ID가 맞지 않으면 `BLOCKED`입니다. 일반 `run` 명령은 이 입력이 없으면 기존처럼 전체를 검사합니다.
선택 과정은 백엔드·예제 소스와 그 성공 판정 조건을 수정하지 않습니다.

## push 이후 결과 확인과 main 병합 제한

작업 브랜치 push → GitHub Actions에서 자동 선택·검사 → 결과 기록 순서입니다. 실패한 원격 커밋을 자동으로 삭제하거나 되돌리지 않습니다. 수정 후 다시 push하면 재검사합니다.

저장소 Actions에서 해당 실행의 실패한 작업·단계 로그를 확인합니다. 실행의 Artifacts에는 상세 로그·선택 이유·화면 캡처·CPU 실패 입력이 남습니다. 실행 환경을 확보하지 못한 BLOCKED는 기능 오류와 구분합니다.

main 병합을 막으려면 관리자가 main 대상 규칙에 PR 필수와 **Require status checks to pass → CI Required**를 설정해야 합니다. 실제 규칙 적용 여부는 저장소 설정에서 확인해야 하며, workflow 파일만으로 병합 차단이 설정되지는 않습니다.

## 사용

Python 3.10+, CMake 3.20+가 필요합니다. CPU 검사는 Ninja와 Clang을 사용합니다. Windows GPU 빌드는 Visual Studio 2022 C++/Windows SDK, Vulkan은 Vulkan SDK 1.4가 필요합니다. LLVM이 PATH에 없으면 `DY_CI_LLVM`에 설치 루트(bin의 부모)를 지정합니다.

```sh
# 작업 폴더를 수동 검증: 커밋하거나 push하지 않음
python -B .github/ci/ci.py run --phase cpu --api null
python -B .github/ci/ci.py run --phase runtime --api vulkan
python -B .github/ci/ci.py run --phase runtime --api d3d12

# 지정한 커밋만 검사
python -B .github/ci/ci.py full --api auto --revision HEAD

# 실패한 CPU 입력 재현
python -B .github/ci/ci.py replay --case <case.json>
```

`The pushed commit does not contain the CI runner; no working-copy fallback is allowed`는 검사할 커밋에 CI 파일들이 아직 없다는 뜻입니다. 개발 중에는 `run`으로 검증합니다. `full`/hook이 미커밋 파일로 대신 통과하도록 우회하지 않습니다.

`full`은 `build-ci/push/source`에 커밋을 동기화하고 동일 파일의 수정 시간을 보존합니다. CPU 검사 후 네이티브 API별 Debug/Release 빌드와 Debug 실행을 검사하며 Debug 빌드를 재사용합니다. Windows auto는 Vulkan/D3D12, Linux는 Vulkan, macOS는 Metal입니다. 의존성은 검사할 커밋의 CMake GIT_TAG를 따릅니다. `run --dependencies build/_deps`는 개발 중 기존 의존성 소스 재사용용이며 `full`에는 주입하지 않습니다.

`install-hook`/`pre-push`는 이전 방식의 명령이며 현재 운영 방식에서는 설치하거나 연결하지 않습니다. 다른 개발자 PC에 이전 dy_engine pre-push가 설치돼 있다면 해당 hook도 해제해야 합니다. clone만으로 hook이 설치되지는 않습니다.

## 실제 검사 내용

| 구분 | 성공 조건 |
|---|---|
| 빌드 | 지원 선언에 맞는 예제·셰이더·검사 실행 파일이 빌드되고 존재함 |
| 원본 HelloWindow | 창 응답, 다른 시점의 실제 클라이언트 화면 2장, 정상 닫기 |
| 원본 그래픽 예제 | 위 조건 + HUD 밖의 장면 영역에 배경과 다른 내용이 존재함 |
| 원본 RenderPath | 기존 per-draw/batched/bindless CLI 각각 장면이 보임 |
| 원본 RenderGraph | 기존 출력의 실제 콜백 Shadow → MainForward → PostProcessing 순서 |
| 별도 GPU 검사 | Clear 빨강, 삼각형 빨강/배경 파랑, 깊이 순서 초록/빨강, 텍스처 RGBW가 지정 좌표에서 일치함 |
| GPU 자원 검사 | 공개 생성/해제 카운터 균형, 버퍼 map/unmap, 정상 종료 |
| 별도 CPU 검사 | 옵션·애니메이션·모델 로더·RenderGraph 기대 결과, 경계 입력, 재현 가능한 변이 입력 |
| 검사 자체 | 잘못된 픽셀·빈 장면·강제 종료·증거 부족·제품 소스 변경이 성공으로 처리되지 않음 |
| 소비자 분리 | 실제 외부 프로젝트가 엔진에 링크해 실행되고 CI/예제가 자동 등록되지 않음 |

예제 관찰은 모든 그림자·조명·애니메이션의 정확성을 증명하지 않습니다. 그림자가 없어져도 다른 도형이 보이면 현재 장면 존재 검사는 통과할 수 있습니다. 별도 GPU 검사 성공을 해당 예제의 기능 전체 성공으로 바꿔 보고하지 않습니다.

CPU sanitizer는 Windows에서 ASan, Linux/macOS의 지원 Clang에서는 ASan+UBSan입니다. GPU 예제에는 sanitizer, 소스 계측, 테스트 전용 매크로를 주입하지 않습니다.

## 환경과 판정

Windows는 로그인된 대화형 화면이 필요합니다. 실행한 프로세스의 창만 관찰하고 닫습니다. 창이 가려지거나 전면 창 확보에 실패하면 `BLOCKED`입니다. 같은 화면에서 GPU 검사를 동시에 실행하지 마십시오. 전용 runner의 로그인 세션을 사용하십시오.

Linux는 창 관리자가 없는 전용 X11/Xvfb 화면을 지원합니다.

`examples.json`의 `warmup`은 첫 응답 이후 캡처를 시작하기 전의 외부 대기 시간입니다. 모델 로딩/기존 벤치마크가 무거운 LoadModel·RenderPath는 5초, 다른 렌더 예제는 1초를 기다린 뒤 3초를 관찰합니다. 프로그램의 시간·코드·기능을 바꾸지 않습니다. 느린 runner에서 첫 화면이 늦으면 로그와 캡처를 확인하여 이 값을 조정해야 합니다.

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
xvfb-run -a -s '-screen 0 1920x1080x24' \
python -B .github/ci/ci.py run --phase runtime --api vulkan
```

소프트웨어 Vulkan은 물리 GPU/드라이버 전체를 대표하지 않습니다. macOS 외부 창 관찰은 아직 미구현입니다. Metal 빌드 경로/테스트 셰이더는 있지만 현재 Metal runtime은 `BLOCKED`이며 실제 Mac 검증도 완료하지 않았습니다. 따라서 모든 플랫폼의 `CI Required`가 통과한다고 주장할 수 없습니다.

- `PASS`: 해당 항목에 선언한 실제 관측/판정 조건 충족.
- `FAIL`: 기대 결과 불일치, 비정상 종료, 응답 정지, 시간 초과 등.
- `UNSUPPORTED`: 예제 CMake가 명시한 API/실행 제외 범위.
- `BLOCKED`: 창 캡처·도구·필수 판정 조건 등 검증 조건 부족. 전체 성공으로 처리하지 않음.

강제 종료는 성공이 아닙니다. 종료 코드 0이어도 화면 2장과 관찰 시간이 없으면 그래픽 검사 성공이 아닙니다. 표준 출력에 나온 GPU 오류는 검출하지만 출력되지 않는 드라이버 오류나 GPU 내부의 모든 제출·표시·누수를 확인했다고 주장하지 않습니다.

## 예제·검사 추가

`examples/*/CMakeLists.txt`를 자동 발견합니다. API 지원 범위는 CMake, 외부 관찰 조건만 `.github/ci/examples.json`에 작성합니다. 새 예제의 프로필이 없으면 빌드는 발견하지만 runtime은 `BLOCKED`로 알립니다. 소스 코드를 보고 성공 의미를 자동 추측하지 않습니다.

일반 예제는 `add_executable`과 `dy_setup_example(target)`을 사용합니다. 예외만 `dy_example_support(KIND cpu)`, `APIS vulkan`, `BUILD_ONLY metal REASON "..."` 등으로 선언합니다. 지원 API의 셰이더가 없거나 `_vs/_ps/_cs` 단계가 불명확하면 실패합니다. Models/Textures는 변경·삭제까지 실행 폴더에 동기화합니다.

정밀 기능 검사가 필요하면 공개 API를 사용하는 작은 검사에 입력과 기대 결과를 작성합니다. CPU `*Checks.cpp`는 CMake가 자동 발견합니다. 공통 CLI는 `--list-scenarios`, `--all`, `--scenario NAME --seed UINT --seconds 0..60 --case-dir DIR`, `--replay FILE`입니다. 변이 검사는 영역별 기본 3초, 최대 60초이며 실제 실패 입력과 바이너리/소스 지문을 저장합니다. 재현 시 지문이 다르면 `BLOCKED`입니다.

## 결과 보관

빌드 폴더의 `ci-logs`에 `result.json`, `junit.xml`, `summary.md`를 생성합니다. `observations/<실행명>-<고유번호>/`에는 원본 PPM 2장, `process.log`, `observation.json`, `checks.json`을 보관합니다. 판정 JSON은 공식 예제와 독립 API 검사를 구분합니다. 검사 전후 `src/`와 `examples/` 소스·셰이더 해시를 비교합니다.

GitHub artifact는 7일 보관합니다. 물리 GPU runner 라벨은 `dy-ci-windows-gpu`, `dy-ci-macos-metal`이며 프로젝트 저장소에만 연결합니다. runner 등록과 저장소 필수 상태 검사 설정은 별도 운영 작업입니다. 이 파일이 runner를 등록하지 않습니다.
