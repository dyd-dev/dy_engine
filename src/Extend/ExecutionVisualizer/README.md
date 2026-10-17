# dy_engine Execution Visualizer

별도 Windows 창에서 재빌드된 계측 제작물의 실시간 데이터 흐름을 관찰합니다. 대상 제작물은 자신의 원래 창에서 실행합니다.

## 빠른 시작: 14번 인스턴싱 비교

첫 예제는 저장소의 [`14_InstancingBatching`](../../../examples/advanced/14_InstancingBatching)입니다. 같은 위치·색의 256개 인스턴스를 인스턴싱 ON에서 Draw 1회, OFF에서 Draw 256회로 묶어 그립니다. 인스턴싱을 분명하게 비교하는 RHI 예제이며, Renderer·Canvas·Model의 다른 기능은 해당 경로를 사용하는 제작물에서 확인합니다.

Windows 준비물은 Visual Studio 2022의 **C++를 사용한 데스크톱 개발**(MSVC x64·Windows SDK), Git, [CMake 3.21 이상](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2017%202022.html), Vulkan SDK입니다. Git/CMake는 PATH에서, `dxc.exe`·`glslc.exe`는 Vulkan SDK의 Bin에서 찾을 수 있어야 합니다. SDK 설치 뒤 새 PowerShell을 열어 `VULKAN_SDK` 환경 변수를 반영하고 Vulkan 지원 그래픽 드라이버를 준비합니다. 최초 configure는 GLFW·stb·ImGui 소스를 다운로드하므로 인터넷 연결이 필요합니다.

아래 명령은 **저장소 루트**에서 실행합니다. GUI는 D3D12·계측 OFF, 14번은 Vulkan·계측 ON으로 각각 새 폴더를 사용합니다. 기존 다른 backend/옵션의 빌드 트리와 섞지 않습니다. ignored `tests/Fixtures`나 기존 로컬 dependency cache 경로는 필요하지 않습니다.

```powershell
cmake -S src/Extend/ExecutionVisualizer -B build/demo-viewer-d3d12 `
  -G "Visual Studio 17 2022" -A x64 -DUSE_D3D12=ON -DUSE_VULKAN=OFF `
  -DDY_ENABLE_LIVE_PROFILING=OFF -DDY_ENABLE_LIVE_CONNECTION=OFF -DDY_ENABLE_TRACY=OFF -DDY_ENABLE_PIX=OFF `
  -DDY_ENABLE_RENDERDOC=OFF -DDY_VIEWER_BUILD_TESTS=ON
cmake --build build/demo-viewer-d3d12 --config Release --target dy_execution_visualizer `
  --parallel 1 -- /nodeReuse:false

cmake -S examples/advanced/14_InstancingBatching -B build/demo14-vulkan `
  -G "Visual Studio 17 2022" -A x64 -DUSE_VULKAN=ON -DUSE_D3D12=OFF `
  -DDY_ENABLE_LIVE_PROFILING=ON -DDY_ENABLE_LIVE_CONNECTION=ON -DDY_ENABLE_TRACY=OFF -DDY_ENABLE_PIX=OFF `
  -DDY_ENABLE_RENDERDOC=OFF
cmake --build build/demo14-vulkan --config Release --target AdvancedInstancingBatching `
  --parallel 1 -- /nodeReuse:false

& .\build\demo-viewer-d3d12\Release\dy_execution_visualizer.exe
```

1. GUI의 `실행 설정`에서 `build/demo14-vulkan/Release`를 검색하고 `AdvancedInstancingBatching.exe`를 선택합니다.
2. 기본 `--mode instanced`를 확인하고 Launch합니다. frames는 기본 생략되어 창을 닫을 때까지 실행하며 `--features`는 사용하지 않습니다.
3. 연결 후 Controls에서 **인스턴싱**을 선택하고 요청값 체크를 바꾼 뒤 **적용 요청**을 누릅니다. 실제 적용과 완료된 1초 구간을 기다려 Draw·Batch 1/256, Instances 256, CPU 기록·원본 흐름을 비교합니다. 두 모드의 화면은 같은 그림을 유지해야 합니다.

기본 FIFO는 FPS를 표시 주기 상한에 묶을 수 있으므로 CPU 기록 시간과 Draw/Batch 변화를 먼저 봅니다. 종료 시 `cpu_record_ms`는 전체 실행의 CPU 기록 시간 합, `avg_record_us`는 프레임 평균입니다. 첫 업로드와 Live 계측 비용을 포함하고 native 변환·GPU 실행·Present·캡처는 제외합니다. 실행 중 모드를 바꿨다면 이 누적값에 두 모드가 섞입니다. 모드별 출력·누적값을 따로 확인하려면 같은 frame 수로 각각 실행할 수 있습니다.

```powershell
& .\build\demo14-vulkan\Release\AdvancedInstancingBatching.exe --mode instanced --frames 120 --capture build/demo14-vulkan/instanced.ppm
& .\build\demo14-vulkan\Release\AdvancedInstancingBatching.exe --mode draws --frames 120 --capture build/demo14-vulkan/draws.ppm
```

## 개발용 빌드·계약 검증

저장소 root에서 `DY_BUILD_EXECUTION_VISUALIZER=ON`을 선택하거나 이 디렉터리를 독립 CMake 프로젝트로 빌드합니다. GUI 렌더용 D3D12/Vulkan backend는 분석 대상의 backend와 독립적입니다. 새로운 UI/차트 라이브러리를 추가하지 않고 기존 `dyf::Gui`, Dear ImGui docking, GLFW를 사용합니다.

```powershell
cmake --build build/demo-viewer-d3d12 --config Release --target `
  ViewerProcessTests ViewerLaunchOptionsTests ViewerStateTests ViewerFlowPlaybackTests ViewerGlfwWindowOwnershipTests ViewerNativeIpcTests `
  --parallel 1 -- /nodeReuse:false
ctest --test-dir build/demo-viewer-d3d12 -C Release --output-on-failure
```

공개 통신 형식은 v3입니다. GUI와 분석 대상 양쪽을 같은 최신 계약으로 재빌드해야 합니다. 분석 대상은 `DY_ENABLE_LIVE_PROFILING=ON`으로 수집하고 별도 `dyf::LiveClient` 연결을 명시적으로 시작해야 합니다. 위 14번 예제는 `DY_ENABLE_LIVE_CONNECTION=ON`일 때 main의 `LiveConnectionScope`가 시작·종료를 맡습니다. 수집만 ON으로 설정하면 앱 내부 스냅샷만 제공하며 서버를 자동 시작하지 않습니다. GUI 자체는 계측 OFF에서도 공통 Client를 사용하며, 장치 생성 전에 `SetLiveCollectorEnabled(false)`를 호출합니다. GUI의 자원·Draw가 대상 목록과 통계에 섞이지 않습니다.

## Windows GLFW 창 소유권 보호

GLFW의 modifier-key 보정에서 active HWND의 `GLFW` 프로퍼티를 직접 역참조하면 다른 프로세스나 GLFW 인스턴스의 포인터를 사용할 수 있습니다. 입력 큐를 연결하면 스레드 간 focus window를 공유할 수 있습니다([Microsoft AttachThreadInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-attachthreadinput)). 이 경로는 실제 Release 충돌의 덤프·원본과 코드가 동일한 link map·대상 HWND 프로퍼티 대조로 확인했습니다. 창 resize 자체가 원인이라는 증거는 없습니다.

[`PatchGlfwWin32.cmake`](../../../cmake/PatchGlfwWin32.cmake)는 build 디렉터리에만 `win32_window.c`를 생성하고 GLFW target의 해당 source 하나를 교체합니다. modifier 보정은 현재 GLFW 인스턴스의 `_glfw.windowListHead`에서 HWND를 찾아 수행합니다. 공유 fetch 원본과 `FETCHCONTENT_SOURCE_DIR_GLFW` 원본은 수정하지 않습니다. upstream 조회 구문이 바뀌면 configure를 중단해 패치 재검토를 요구합니다. GUI와 대상 프로그램 모두 다시 configure/build해야 이 보호를 받습니다.

## 사용

- 제목 바로 아래의 `Overview / Flow / Targets / Controls / Events / Objects / GPU / History / Requests / Details` 선택 줄에서 각 보기를 켜고 끕니다. 기본 배치는 위쪽 Overview 30%와 넓은 Flow입니다. Overview 카드는 스크롤할 수 있습니다. 체크한 보기만 `선택 보기 좌우`로 같은 폭의 열에 놓거나 `선택 보기 탭`으로 한 영역에 겹칩니다. 2개를 체크하면 한 창 안에서 나란히 배치되며, 1개는 전체 영역을 사용하고 0개일 때 두 버튼은 비활성입니다. 창의 탭/제목 드래그도 유지합니다. `배치 초기화`는 기본 Overview 위/Flow 아래 배치로 되돌리며 보기 체크 상태는 유지합니다. 버튼 줄은 창 폭에 맞춰 줄바꿈합니다. 선택 보기를 처음 열면 줄바꿈까지 반영한 헤더 아래에서 시작하고, 이후 사용자가 이동·도킹한 위치를 유지합니다. ImGui 다중 OS viewport는 켜지 않습니다.
- `Targets`: 같은 로그인 사용자/세션의 제작물을 자동으로 발견합니다. 목록에서 선택하면 관찰이 시작됩니다. `Disconnect`는 제작물을 종료하지 않습니다. 기본 상태는 `갱신중 / 대기 / 연결종료`이며 마지막 측정값을 보존합니다. 계속 증가하는 frame/sequence/age는 기본 화면에 표시하지 않고 상세 tooltip과 보고서에 둡니다.
- `Targets`의 `Launch setup / 실행 설정`을 열고 폴더를 입력하거나 Windows 폴더 선택기로 고릅니다. `Scan / 하위 폴더 재검색`을 누르면 worker가 모든 하위 폴더의 `.exe`를 검색합니다. CompilerId·로그 보조 exe·Debug/Release를 포함하며 예제 필터는 없습니다. 개수와 상대 경로를 표시하고 긴 목록은 스크롤합니다. 이름이 같아도 상대 경로로 구분합니다.
- 목록에서 exe를 선택하면 오른쪽에 현재 소스 기준의 인자 이름·종류·값이 나타납니다. 포함할 항목을 체크하고 enum을 고르거나 정수/실수를 옆 값칸에 입력합니다. 일반 예제 18개에는 공통 인자를 붙이지 않고, 도움말 전용 예제와 `--frames=N` 문법의 ImGui 예제를 구분합니다. 현재 소스와 다른 빌드·동명 외부 프로그램에는 `Raw command`를 사용합니다. 도움말이나 파일을 검색 중 자동 실행하지 않습니다.
- 미등록 exe는 인자 이름·종류(`flag/text/integer/float`)·값 행을 추가/삭제합니다. 이름 끝이 `=`이면 `--name=value` 한 토큰으로 결합하고, 나머지는 이름/값을 별도 토큰으로 인용합니다. 숫자 범위·enum·near/far 및 일부 상호 배타 조건 오류는 표시하고 실행을 막습니다. Launch/Close와 오류는 스크롤 본문 밖 footer에 두며, 긴 오류는 한 줄로 표시하고 hover로 전문을 읽습니다. 값의 공백·따옴표는 기존 `QuoteArgument`로 처리합니다. `Raw command`는 기존 Windows 인자 문자열을 직접 넣는 호환 경로입니다.
- `LiveExternalConsumer.exe`는 `--features`만 기본 활성화하는 알려진 프리셋입니다. 인자 없음은 최소 RHI 검증이고, `--features`는 Renderer + Canvas + RHI 최적화 비교이며 Model은 별도 제작물이 필요합니다. `--frames`는 0~4294967295(0=무제한)이고 기본 생략하므로 창을 닫을 때까지 실행합니다. `--verify`는 기본 OFF이며 켜면 frames를 features 9 / renderer 16 / raw·canvas 8로 덮어쓰고 자동 검증 후 종료합니다. features+renderer는 허용하고 features+raw/canvas는 순서와 관계없이 거절합니다. features가 꺼져 있으면 raw/renderer/canvas는 미리보기의 마지막 인자를 적용하는 실제 파서 동작을 유지합니다. 가상 `--mode`·`--help` 등은 제공하지 않습니다. 이 기본값은 GUI 프리셋에 적용하며, Raw command와 CLI의 직접 문자열에는 자동 추가하지 않습니다.
- 전체 명령을 확인하고 `Launch / 선택 exe 실행`을 눌러 실행합니다. 선택 시 작업 폴더는 exe의 parent이며 다른 exe로 바꾸면 기존 인자·직접 문자열·작업 폴더를 초기화합니다. 필요하면 작업 폴더를 수정하고, 빈 값이면 exe parent를 사용합니다. `직접 실행 파일 경로`도 제공하며 기존 CLI `--target/--arguments/--working-directory`의 명시적 실행을 유지합니다. 실행은 기존 `TargetProcess::Launch/CreateProcessW`를 사용합니다.
- 긴 검색은 UI 스레드와 분리되며 취소할 수 있습니다. 접근 거부·삭제 경쟁은 부분 검색 오류로 표시하고, 순환 방지를 위해 링크/junction 하위 폴더는 생략해 개수·경로를 알립니다. 오류/생략 상세는 처음 32건을 표시합니다. 폴더를 바꾸면 이전 검색을 취소하고 오래된 결과를 버립니다. 소유 실행 프로세스가 살아 있으면 새 Launch를 막으며 대상 프로세스를 강제 종료하지 않습니다.
- `Source`: 장치/Renderer/raw-RHI source를 선택합니다. 상단 값, 흐름, 추이, 옵션이 같은 source를 가리킵니다.
- `Flow`: 기본 화면은 선택한 실제 기록 경로 하나만 그립니다. 참조·의존 경로는 `참조·의존 경로 펼치기`로 목록에 포함하고 별도로 선택합니다. `원본 항목`은 모든 보존 항목과 실제 연결 ID를 확인하는 진단 목록입니다. 모든 관계를 한 canvas에 겹쳐 그리지 않습니다.
- 실제 from/to가 이어지는 경로를 occurrence별로 배치합니다. Record→Upload→Record의 재등장은 별도 위치이며, 연속 같은 단계 반복만 요약합니다. 순방향은 직선, 자기·되돌림·참조 관계는 전용 직각 통로를 사용합니다. 이웃 segment의 출입 포트와 세로 구간을 분리하고, 선 위 연결 개수 문구는 제거했습니다. 인접 segment는 서로 다른 x 구간을 사용하므로 여러 되돌림도 같은 높이를 공유하며 canvas 높이는 320px로 유지합니다. canvas child는 실제 남은 공간만 사용합니다. 단계명·반복 설명은 선보다 위에 두고 단일 마커도 숫자·단위 아래 통로를 따릅니다.
- `3초 흐름 재생 / 실제 실행속도 아님`: 같은 immutable snapshot·원본 frame key·revision·관계를 3초간 유지합니다. 선택 경로만 순차 이동시키며 다음 주기에 새 대표 프레임을 채택합니다. 경로를 직접 선택하면 같은 원본에서 3초 재생을 다시 시작합니다. 분기/합류는 경로를 나누고, ID 정렬·BFS·timestamp로 전역 실행 순서나 분기 결과를 추정하지 않습니다. 원본에 없는 NativeTranslate→Submit 또는 GPU→Output 연결을 추가하지 않습니다.
- occurrence 원을 클릭하면 해당 occurrence의 실제 원본 ID를 선택합니다. 같은 단계에 먼저 나온 다른 항목을 대신 선택하지 않습니다. 같은 단계가 다시 나타날 때 표시한 단계 평균은 같은 생산자 평균이며 합산하지 않습니다. Buffer/Texture 생존 목록은 Upload 작업으로 간주하지 않고, 현재 흐름의 GPU는 같은 device/source/frame/revision만 허용합니다.
- 본문 22px, 보조 20px, 단계명 26px, 주요 숫자 34px를 사용하고 입력칸·행·여백도 함께 넓힙니다. 카드 열 수는 창 폭에 맞추며 최소 폭보다 좁으면 스크롤합니다. 큰 수는 글꼴 축소 대신 지수 표기를 쓸 수 있고 원본 수치는 보고서에 보존합니다. 원본 ID·calibration ns 등은 상세/tooltip에 둡니다.
- `Controls / 실제 적용` 안의 최적화 분석은 실제 등록된 기능 목록에서 하나를 선택합니다. 제작물의 `ControlDesc` 설명·ON/대체 경로·적용 조건·추가 비용을 읽고, 실제 적용 값과 요청 초안을 구분합니다. 최적화 / 품질·표시 조건 / 빌드·고정 구조 / 미분류를 구분하고 읽기 전용·재시작·재빌드 사유를 유지합니다. 알려진 key의 한국어 제목은 붙일 수 있지만, 미등록 제어나 대체 구현 없는 OFF를 만들지 않습니다.
- Controls 목록 제목의 기능 개수는 현재 device/source에 실제로 등록된 항목 수입니다. 실행 모드와 source 선택에 따라 달라지며 예상 개수나 미등록 기능으로 채우지 않습니다.
- 최근 실제 Applied 요청과 선택한 기능이 일치할 때 이전·이후 생산자 고정 1초 구간을 비교합니다. baseline이 없으면 준비 부족으로 남깁니다. CPU 준비·데이터·업로드·기록·native 변환·제출·GPU·출력의 CPU/GPU 시간, 작업·요소·바이트를 선택해 변화량과 비율을 읽습니다. CPU 측정 프레임 수를 함께 표시하며 scope 평균의 합을 프레임 wall time으로 해석하지 않습니다. 준비되지 않은 GPU, 부분 GPU, 잘못된 구간은 `--`이며 0으로 채우지 않습니다.
- 프로그램에서 설정을 직접 바꿔 source의 현재 revision이 더 높아져도 요청에 보존된 정상 after 구간을 표시합니다. 실제 적용 key·요청 revision·완료된 원본 1초 구간을 검증하며 `과거 비교 rev 2 / 현재 rev 3`처럼 구분합니다. 현재 revision의 평균으로 이전 요청의 수치를 바꾸지 않습니다.
- 작업 비교는 기록 / 제출 / 완료를 따로 선택합니다. Draw·DrawIndexed·Dispatch·instances·barrier·binding·제출·대기·생성·해제·배치·처리 요소·변경 구간·upload 호출/바이트·전체/부분 upload와 실제 native 업로드 페이지 할당/재사용/할당 바이트의 생산자 평균을 표시합니다. 페이지 내 slice는 페이지 재사용으로 세지 않는 생산자 계약을 따릅니다. 장치 누적 업로드 풀 gauge를 source별 1초 수치로 재해석하지 않습니다.
- 전후 흐름은 `comparisonEvidence`의 실제 원본 두 개를 같은 화면에 나란히 그립니다. 원본 frame/device/source/revision과 대표 프레임의 1초 구간을 검증하고 한 쌍을 함께 3초 고정합니다. 각 선택 경로에 마커 하나를 사용하며 원 안 숫자는 그 원본의 작업 수(GPU는 span 수)입니다. 평균 표는 계속 1Hz로 갱신하므로 원본 구간과 현재 평균 구간이 다르면 별도 구간으로 명시합니다. 같은 phase는 실행 시간 정렬이 아닙니다. 새 요청·source·세션에는 이전 쌍을 붙이지 않습니다.
- 원본 상세의 단계 이벤트/GPU span·기록 명령 종류·단계 사이 연결을 관측 개수로 비교합니다. ID 차집합·정확한 역할 대응·정적 예상 화살표를 만들지 않습니다. 유실·보존 한도·불완전 상세는 사유를 남기며 감소량/비율로 부재나 사라짐을 판정하지 않습니다. 유실 없는 원본에서도 관측 종류가 같다고 같은 의미의 작업임을 단정하지 않습니다.
- 명령 종류 표는 논리 Command·Upload와 Wait·Present의 관측 이벤트를 셉니다. UpdateBuffer/Texture의 Upload를 포함하고, Wait는 명령 종류가 비어 있는 실제 대기 이벤트도 포함합니다. 네이티브 번역·페이지 획득·제출 완료 표식과 acquire 생략 이벤트는 중복 작업으로 세지 않습니다. 완료 횟수나 GPU 명령 실행 횟수를 뜻하지 않습니다.
- 실제 설정의 전후 값(`previousActual`/`actual`)을 표시하며 동일 값 요청은 기능 전환 효과로 해석하지 않습니다. 단일 변경과 동일 부하를 확인하지 않은 결과는 '선택 구간의 관측 변화'입니다. 비교 표·기능 목록·원본 canvas는 각각 스크롤하고 22px 본문을 유지합니다. 폴더 런처와 기존 Flow의 3초 재생은 같은 구현을 유지합니다.
- `Overview`: 완료된 생산자 고정 1초 구간의 FPS·프레임·CPU·GPU·Draw·instances·batch·upload·의미 객체 평균만 초당 1회 표시합니다. `Low / High`는 같은 세션/source 실행 중 완료된 **1초 평균**의 최저/최고이며 개별 프레임 max가 아닙니다. revision 전환이 섞인 구간은 혼합 표시를 보존합니다. 미수집·대기·미지원은 0 대신 `--`와 원래 상태로 표시하고, FPS도 유효 프레임 시간 없이 0으로 추정하지 않습니다.
- `History`: `recentSeconds`에 담긴 최근 10초 내 완료 1초 구간 최대 10개를 그대로 복사해 초당 1회 그립니다. 늦은 연결·재연결 때도 생산자가 보존한 구간을 복구합니다. GUI의 수신 샘플이나 100ms history를 재평균하지 않으며 누락·미측정 구간과 revision 전환을 이어 그리지 않습니다. 동일 endNs에 늦게 도착한 GPU 보정도 다음 1Hz 갱신에서 반영합니다.
- 1초 통계 갱신과 3초 흐름 재생은 별개입니다. 흐름의 원형/연결이 유지되는 동안 원형 안의 평균 숫자는 다음 완료 1초 통계로 갱신될 수 있습니다. 참조 자원 수는 고정된 대표 프레임의 원본 개수로 명시합니다. 새 세션/source는 두 표시 상태를 초기화합니다. 같은 세션/source 재연결의 Low/High는 생산자 실행 범위를 유지합니다.
- GPU 선택은 같은 sample ID의 늦은 Pending→Ready 결과를 따라갑니다. 실제 원래 frame/revision·제출·queue·clock domain을 보존합니다. CPU 배치는 해당 span의 실제 보정이 있을 때만 표시합니다. frame ID 차이를 지연 프레임 수로 계산하지 않습니다. 선택한 원본 이벤트의 시간·인자는 Details의 진단값이며 Overview의 평균과 구분합니다.

## 검증용 인자

```text
--target <UTF-8/Unicode exe path>
--arguments <Windows argument string>
--working-directory <directory>
--attach-pid <running instrumented PID>
--frames <positive count>
--capture <GPU-readback PPM path>
--report <text report path>
```

`--frames`가 없으면 창을 닫을 때까지 실행합니다. `--capture`만 지정하면 90프레임 실행합니다. readback은 GUI backbuffer의 확인용이며 OS 창 관찰과 별도 증거입니다.

보고서는 실제 Present 수·수신 수·`received_*` 최신 원본을 진단용으로 기록하고, 실제 표시된 완료 1초 구간(`statistics_last_presented`)과 3초 재생의 고정 frame/revision/cycle(`flow_last_presented`)을 따로 기록합니다. 통계 구간의 생산자 평균/실행 범위와 흐름의 sequence는 서로 다를 수 있습니다. 숨겨진 화면의 최신 수신값을 표시했다고 기록하지 않습니다. 마지막 Present의 가시성과 표시 갱신/주기 수를 함께 보존합니다. `statistics_metric`은 표시된 구간의 생산자 측정값을 기록하며 metric별 OS 가시성을 측정하는 것은 아닙니다.

`comparison_last_presented`는 실제 표시한 request/control·적용 frame/revision·원본 snapshot sequence를, `comparison_before/after`는 원본 1초 구간과 준비 여부를 기록합니다. `comparison_metric`은 실제 표시한 평균·변화량·변화율과 셀 가시성 비트(1=전, 2=후, 4=차이, 8=비율)를 보존합니다. 숨겨진 표의 최신 수신 status로 표시 기록을 갱신하지 않습니다. GPU 부분 측정과 표시되지 않은 셀은 숫자 대신 `--`로 남깁니다.

`comparison_stage_last_presented`와 `comparison_counter_last_presented`는 해당 표를 마지막으로 실제 표시한 request·sequence·전후 원본 구간을 별도로 보존합니다. `comparison_stage`는 단계/항목 index와 평균·변화량·비율·셀 가시성, `comparison_stage_coverage`는 측정/대기/미지원/미수집 프레임 수입니다. `comparison_counter`는 기록(0)·제출(1)·완료(2) 범위와 field key를 보존하며 native 페이지 획득 3개는 기록에서만 숫자를 냅니다. 다른 섹션으로 스크롤한 뒤 수신된 새 평균으로 과거 표시 증거를 덮어쓰지 않습니다.

`comparison_revision_context`와 표별 `comparison_stage/counter_revision_context`는 비교 요청의 실제 적용 revision·표시 당시 source의 현재 revision·과거 비교 여부를 기록합니다. 과거 after의 준비 여부는 현재 revision과 같아야 한다는 조건으로 숨기지 않습니다.

`comparison_flow_last_presented`는 원본 쌍의 요청·sequence·3초 cycle·Present 시각을, `comparison_flow_frame`은 두 원본 key/revision/window·사용 가능/정합성/상세 완전성·현재 평균과 같은 구간인지·canvas 가시성을 기록합니다. `comparison_flow_loss`는 유실을, `comparison_flow_route`는 선택 경로의 실제 연결/단계 수와 화면에 보인 occurrence ID를 보존합니다. 원본의 단계·기록 명령·단계 사이 연결 표는 각각 `comparison_flow_stage/command/link`로 남기며, 불완전 상세의 변화량·비율은 `--`입니다. 이 표시 기록은 최신 하나씩으로 제한되며 모든 요청의 상세 프레임을 복제하지 않습니다. `comparison_known_revision_change`는 같은 revision에서 알려진 실제 처리 요청의 전후 값을 남깁니다.

기존 적용→표시 p95 ≤250ms 판정은 사용자가 승인한 **1Hz 통계 + 3초 관계 재생**으로 대체되었습니다(`legacy_250ms_gate=retired_by_user`). 새 보고서는 해당 p95를 계산하거나 판정하지 않습니다. 아래 과거 바이너리의 실측값과 원본 로그는 이력으로 보존합니다.

## 계약 검증

- `ViewerProcessContracts`: Windows 인자 round trip, 잘못된 UTF-8/NUL 거절, 한국어 exe·작업 폴더·인자로 실제 자식 프로세스 실행, 실패 시 기존 종료 결과 보존.
- `ViewerLaunchOptionsContracts`: 재귀 검색·동명 exe/대소문자·한글/공백·빈/잘못된 경로·취소, exe 선택 간 인자 격리, 숫자/enum/관계 제약, `=` 문법과 실제 자식 프로세스 argv/cwd를 검사합니다. 이번 Windows 실행에서 directory symlink fixture 생성은 권한 부족으로 SKIP했습니다. 실제 `build/live-rhi-d3d12-on` 검색은 오류 없이 17개를 반환했고 [전체 상대 경로](../../../build/live-root-review/launcher-real-catalog.txt)를 보존했습니다.
- `ViewerStateContracts`: immutable snapshot 수명, all-frame 평균 보존, 실제 적용 상태·기준값, typed 범위 검사, 늦은 GPU 결과, 지연 전송의 QPC age, 실제 link 연결 그래프.
- `ViewerFlowPlaybackContracts`: 3초 snapshot 고정/경계 전환, frame/revision/source/session 검증, 1Hz 생산자 평균·Low/High 원본 보존, recentSeconds attach/동일 구간 보정, 과거 GPU 격리, 실제 연속 경로·분기/순환·동일 단계 반복 요약·Record→Upload→Record 보존, 단일 경로의 순차 phase, 직선/직각 이동·독립 중간 원 우회와 실제 표시 보고서를 검사합니다. 같은 Batch ID가 다음 프레임에 재사용되어도 선택한 원본 frame/revision/요소가 바뀌지 않는 회귀는 State 계약으로 확인합니다.
- `ViewerGlfwWindowOwnershipContracts`: 실제 GLFW target의 modifier 블록을 추출해 원본 접근 위반을 SEH로 재현하고, 외부 HWND·별도 인스턴스 거부와 소유 HWND의 정상 modifier 해제를 검사합니다. 창 생성이나 입력은 하지 않습니다.
- `ViewerNativeDriverContracts`: native driver의 인자·식별자·history·재시도 분류 self-check. 실제 native 검증 절차는 [`NativeIpcREADME.md`](tests/NativeIpcREADME.md)를 따릅니다.
- 1Hz/3초 표시 개편과 Batch 선택·독립 중간 원 우회 보완 후 계약 **6/6**이 통과했습니다([결과](../../../build/live-root-review/flow-redesign-contracts.txt), [모델 빌드](../../../build/live-root-review/flow-redesign-model-build.txt)). Batch 원본 선택은 [수정 전 실패](../../../build/live-root-review/flow-batch-selection-red.txt)를 보존했습니다. 표시 주기 개편의 [Release GUI 빌드](../../../build/live-root-review/flow-redesign-build-final.txt)가 성공했고 SHA256은 `3659685D7C7EDD6726FA108EA948FB855EA792C86096352592C9674DF3B1FF95`입니다. 해당 바이너리의 실제 입력 증거는 아래에서 별도로 기록합니다.
- GLFW 수정 후 Release 빌드와 위 네 계약 테스트 4/4가 통과했습니다. root도 [4/4 결과](../../../build/live-root-review/glfw-fixed-viewer-contracts.txt)를 독립 확인했습니다. GUI와 D3D12 example14/Vulkan example15 대상 모두 수정된 GLFW로 재빌드했습니다.
- 폴더 런처 초기 `F82E...` 빌드에서 전체 Viewer 계약 **5/5**가 통과했습니다([선행 실패](../../../build/live-root-review/launcher-contract-red.txt), [통과](../../../build/live-root-review/launcher-contract-green.txt)). 이후 footer를 스크롤 본문 밖에 고정하고 Advanced02/03의 near/far를 대상과 같은 `std::stof` float 변환 후 검사하도록 보완했습니다. float 반올림·underflow·overflow·정상값을 포함한 [LaunchOptions 계약 1/1](../../../build/live-root-review/launcher-float-green.txt)이 통과했습니다([수정 전 실패](../../../build/live-root-review/launcher-float-red.txt)).
- root의 실제 UI 검증에서 `F82E7740C187424E54FCE7191343664D04F8FAB28C4E84A522DA3855B7CCC53F`(GUI PID 7148)는 native 폴더 선택과 미등록 exe 전환 시 인자 초기화·행 추가·종류 선택을 확인했습니다. 이후 `7EC0A349D0CAAC913F4DC5E6747AB78A037525BB2020C014CDE08C165F95FE00`(GUI PID 6004)는 직접 폴더 경로 입력·17개 검색(오류 0)·14 프리셋과 cwd·frames -1의 Launch 비활성화 후 1800 수정·mode draws 선택·기본/오류 상태의 Launch/Close 노출을 확인했습니다. 7EC0에서 선택한 Release14를 `--frames 1800 --mode draws`로 실제 실행한 [CIM 명령 증거](../../../build/live-root-review/launcher-target-command.json)와 자동 연결 Draw/Batches 256, 대상 1,800 frames 후 exit 0, 뷰어 exit 0 및 [GUI 보고서](../../../build/live-root-review/launcher-gui-final.txt)를 보존했습니다.
- 그 뒤 오류 이유가 스크롤 아래에 가려지는 표시만 footer 한 줄+tooltip으로 보완했습니다. 폴더 런처 단계 Release GUI SHA256은 `78A10BD38167D4C11673B0BB5D9DA86419FF60C12D421AD63E2EB14AC3DD7F57`입니다. root가 이 빌드에서 음수 frames의 Launch 비활성화와 주황색 정수 범위 오류가 스크롤 없이 함께 보이는 것을 확인했습니다.
- 최종 빌드에서 Vulkan 폴더 입력·17개 검색(오류/링크 0)·목록 스크롤·Release15 선택·부모 cwd와 mode 프리셋을 확인했습니다. `--frames 1800 --mode create`로 실제 PID 25736을 GUI PID 17368의 자식으로 실행한 [Vulkan CIM 명령 증거](../../../build/live-root-review/launcher-vulkan-target-command.json)를 보존했습니다. root는 별도 대상 창의 컬러 타일 256개 출력과 연결된 Flow의 Draw 1·Batch 1·instances 256·upload 8,192 B·GPU 완료를 확인했습니다. 대상 자연 종료와 GUI 종료 모두 exit 0이며 [Vulkan GUI 보고서](../../../build/live-root-review/launcher-gui-vulkan.txt)를 저장했습니다. D3D14(7EC0)와 Vulkan15(78A10)의 폴더 런처 실제 기능 검증이 완료됐습니다. 마지막 표시 변경에 계약 전체를 반복 실행하거나 기존 p95를 재측정한 것으로 기록하지 않습니다.

## 최적화 분석 개편의 검증 범위

제품 위치는 `src/Extend/ExecutionVisualizer`입니다. 기존 27개 제품 파일과 새 `ComparisonView.cpp`를 이 위치에 모았고, 이동 시 함께 있던 검증 로그 5개는 `build/live-viewer-optimization-d3d12`에 분리 보존했습니다. 이동 순간 33개 파일의 전후 hash 차이는 0건이며 빈 `tools`만 제거했습니다. [이동 전](../../../build/live-root-review/optimization-viewer-move-before.json), [이동 후](../../../build/live-root-review/optimization-viewer-move-after.json), [로그 이동](../../../build/live-root-review/optimization-viewer-log-move.json)에 원본 매핑을 남겼습니다.

최적화 분석의 새 계약은 단계별 생산자 평균·CPU 측정 범위·부분 GPU 배제, 원본 ID 재발급과 관측 종류 구분, 기록 전용 페이지 획득, 원본 쌍의 3초 동결·새 요청/source/세션 분리·완료 구간 검증·유실 시 부재 판정 금지, 스크롤 후 마지막 실제 표시 증거 보존을 검사합니다. v3 [전체 빌드](../../../build/live-viewer-optimization-d3d12/final-build.txt)·[동결 후 확인](../../../build/live-viewer-optimization-d3d12/freeze-build.txt)에 이어, 화살표·최초 창 높이 및 독립 리뷰 보완의 [빌드](../../../build/live-viewer-optimization-d3d12/review-fixes-build.txt)가 성공했고 [전체 계약 6/6](../../../build/live-viewer-optimization-d3d12/review-fixes-contracts.txt)이 통과했습니다. 해당 단계 GUI SHA256은 `4FF9C6E9BFC2D78961DD236FD3F98739485D412EB353ED8162B4250D2D579581`입니다.

consumer 프리셋 검증에 사용한 GUI SHA256은 `57EFA234ECD81B497221328604E53CE1151BBCE309211FD3F9A2F32E59A75E02`입니다. `LiveExternalConsumer.exe`를 미등록 실행 파일로 다뤄 인자 없이 최소 RHI 경로로 시작하던 런처를 보완했습니다. 기본 `--features`와 무제한 실행을 유지하고, 실제 지원 옵션·상호 배타 조건·Model 별도 제작물 안내를 제공합니다. Controls 개수는 실제 필터링된 목록 크기입니다. [수정 전 실패](../../../build/live-viewer-optimization-d3d12/consumer-preset-red.txt), [런처 계약 1/1](../../../build/live-viewer-optimization-d3d12/consumer-preset-contracts.txt), [GUI 빌드](../../../build/live-viewer-optimization-d3d12/consumer-preset-gui-build.txt), [전체 계약 6/6](../../../build/live-viewer-optimization-d3d12/consumer-preset-all-contracts.txt)을 보존했습니다. 프리셋 계약은 기본 인자 직렬화·uint32 경계·두 인자 순서의 모드 조합·직접 문자열 호환을 검사합니다. 실제 기본 선택/Launch·등록 개수·최적화 효과는 root의 새 GUI 검증과 구분합니다. [4FF9 인계](../../../build/live-viewer-optimization-d3d12/handoff-4FF9C6E9BFC2D789.json)에 이전 소스 hash와 검증 이력을 보존했습니다.
root는 `57EFA234ECD81B497221328604E53CE1151BBCE309211FD3F9A2F32E59A75E02` GUI PID 45148에서 실제 폴더 검색·consumer 선택 시 `--features` Flag 기본 체크를 확인하고 그대로 Launch했습니다. [실제 child 명령줄](../../../build/live-root-review/consumer-mode-gui-launched-target.json)은 PID 34248 / parent 45148과 exe `"--features"`를 보존합니다. root의 GUI 관측에서 등록된 기능은 23개였고, instancing true→false 적용 revision 1→2 뒤 표의 DrawIndexed는 1→256, Binding은 16→780, Draw는 4→4, instances는 260→260이었습니다. 해당 GUI는 사용자에게 이 표를 보인 상태로 열려 있어 typed report는 아직 저장되지 않았습니다. 이 수치는 root의 화면 관측으로 구분하며 미생성 report 파일을 증거로 넣지 않습니다. CPU/GPU 시간 개선이나 다른 최적화 전환까지 확인한 것으로 확대하지 않습니다.
후속 보완은 malgun의 실제 U+2192 glyph(549번)를 한국어 범위에 추가하고 22px를 유지하며, 최초 선택 보기 높이를 헤더 아래 남은 viewport 높이로 제한합니다. [수정 전 실패](../../../build/live-viewer-optimization-d3d12/review-regressions-red.txt)는 Upload·Wait·Present 관측 누락과 programmatic revision 뒤 after 숨김을 재현합니다. 논리 Upload·Wait·Present를 세면서 번역·페이지 획득·완료 표식·acquire 생략을 중복 집계하지 않고, 요청의 실제 적용 key/revision/원본 1초 구간을 검사해 과거 비교를 유지하는 회귀가 통과했습니다. 표시 보고서에는 과거 비교와 현재 revision을 따로 남깁니다.

독립 검증이 4FF9 실행을 시작한 시점에 추가 안내 문구 한 줄의 링크가 겹쳐 [LNK1104 실행 잠금](../../../build/live-viewer-optimization-d3d12/review-fixes-final-link.txt)으로 실패했습니다. 실행 파일은 4FF9 해시를 유지했고, 링크되지 않은 추가 문구만 실행본 내용으로 복구한 뒤 제품 소스를 동결했습니다. 이 실패 로그를 마지막 성공 빌드로 취급하지 않습니다. [당시 인계](../../../build/live-viewer-optimization-d3d12/handoff-4FF9C6E9BFC2D789.json)는 4FF9 실행 파일·당시 제품 목록·검증 로그를 가리킵니다.

계약 6개는 컴파일·CPU/모델·NativeDriver self-check이며 실제 GUI 입력·GPU 실행 증거와 구분합니다. directory symlink fixture는 Windows 권한 부족으로 SKIP했습니다. 4FF9의 글꼴·초기 높이·paired flow 실제 확인은 root가 별도로 수행합니다. 아래 기존 SHA/실측은 해당 바이너리의 이력으로 유지합니다.

root는 이전 `36687EE1BE3435DF2C38F0C688991699A87F103E96F3BAEE30F4C4FC4C2DAE95`에서 Vulkan 외부 consumer의 `canvas.imageCache` true→false를 실제 적용했습니다. [GUI 보고서](../../../build/live-root-review/optimization-gui-canvas.txt)의 단계 작업/frame은 Upload 1→3, Record 53→57이며, 이전 revision 1과 이후 revision 2는 각각 혼합 없는 완료 1초·30 frames입니다. [실행 hash](../../../build/live-root-review/optimization-gui-canvas-process.json), [캡처](../../../build/live-root-review/optimization-gui-canvas.ppm), [GUI 6,000 Present 후 exit 0](../../../build/live-root-review/optimization-gui-canvas-exit.json)을 보존했습니다. 해당 실행에서는 paired flow 화면을 아직 확인하지 않았으며, 이 결과를 4FF9의 가독성 보완 검증으로 바꾸어 기록하지 않습니다. [36687 원본 인계](../../../build/live-viewer-optimization-d3d12/handoff-36687EE1BE3435DF.json)에 당시 제품 파일 hash를 분리 보존했습니다.

## 가독성·기능 비교 개편 검증

C 구현의 Release GUI SHA256은 `C0A8A63F6B68B4D8B28086ABF9F1C1C5BA6ACD828E1639BE582DB231B590E2CC`입니다. [GUI 빌드](../../../build/live-root-review/readable-viewer-build.txt)가 성공했고 [계약 6/6](../../../build/live-root-review/readable-viewer-contracts.txt)이 통과했습니다. [수정 전 실패](../../../build/live-root-review/readable-viewer-red.txt)를 보존했습니다. occurrence의 실제 ID·원본 연결 연속성, 직선/직각 segment·출입 포트 비중첩, 문자 영역을 침범하지 않는 단일 마커, 완료 1초/순수 revision 검증, baseline 없는 Applied 표시, 부분 GPU 배제, 실제 표시 비교 기록을 검사했습니다. 이는 컴파일·모델 계약 증거이며 이 바이너리의 실제 GUI 입력 검증과 구분합니다. 공용 collector의 후속 최적화로 원본 채팅이 재링크하면 해당 실행 hash를 별도로 기록해야 합니다. 최초 실제 화면에서 발견한 하단 직각선 클리핑을 보완해 되돌림 높이를 고정하고 기본 Overview 비율을 30%로 조정했습니다. [고정 높이 회귀의 수정 전 실패](../../../build/live-root-review/compact-flow-red.txt)와 [기하 보완 후 계약 6/6](../../../build/live-root-review/compact-flow-contracts.txt)을 보존했습니다. 후속 [Release GUI 빌드](../../../build/live-root-review/compact-flow-viewer-build.txt)가 성공했고 SHA256은 `52DE03AC4EEE330D044ABA055D232E1FB74B3ABFEBF354FFC3FD5C6E525516EE`입니다. 이 후속 바이너리의 기본 1540×960 배치 확인은 실제 GUI 검증과 구분합니다.

원본 채팅은 C0A8 단계에서 실제 컬링 적용으로 Instances 260→132, 인스턴싱 해제로 Draw 5→132를 확인했습니다. [native GUI 보고서](../../../build/live-root-review/features-gui-culling-native.txt)의 마지막 표시 비교는 인스턴싱 해제이며, baseline revision 2와 after revision 3의 원본 구간은 각각 정확히 1초입니다. 해당 비교의 Draw는 5→132이고 실제 표시 가시성 mask도 보존했습니다. [3,600 Present 실행 종료](../../../build/live-root-review/features-gui-culling-native-exit.json)는 exit 0입니다. 이 기능 입력 결과를 후속 높이 보완 빌드의 기본 배치 검증으로 바꾸어 기록하지 않습니다.

## 1Hz·3초 실제 GUI 검증 (2026-10-15, 가독성 개편 전)

root가 `3659685D7C7EDD6726FA108EA948FB855EA792C86096352592C9674DF3B1FF95` GUI를 실제 조작했습니다. C 채팅은 보고서·실행 hash·exit 기록을 대조했고 GUI를 직접 조작하지 않았습니다. 아래 결과는 선택 보기 배치 버튼 추가 전 바이너리의 검증입니다.

- D3D12 example14: 실제 Instancing false→true 적용으로 Draw 256→1과 실행 Low 1 / High 256 보존을 확인했습니다. 큰 평균 카드·보기 선택·활성 단계·단일 마커 재생과 연결 해제/재연결 동작을 확인했습니다. [보고서](../../../build/live-root-review/flow-gui-d3d12-final.txt), [실행 hash](../../../build/live-root-review/flow-gui-d3d12-final.json), [GUI exit 0](../../../build/live-root-review/flow-gui-d3d12-final-exit.json)을 보존했습니다.
- Vulkan example15: Upload 8,192 B/프레임, Ready GPU 측정, 활성 Upload 원, Record→Upload→Record→Submit→GPU 원본 경로의 순차 재생과 직각 회귀를 확인했습니다. Reuse true→false 실제 적용으로 새 자원 생성도 확인했습니다. Data 원에서 고른 Batch ID 23의 원본 frame `81393827` / revision 2 / resource `81393830`은 5초 이상과 다음 재생 주기 후에도 유지됐습니다. [보고서](../../../build/live-root-review/flow-gui-vulkan.txt), [실행 hash](../../../build/live-root-review/flow-gui-vulkan.json), [GUI exit 0](../../../build/live-root-review/flow-gui-vulkan-exit.json)을 보존했습니다.
- 표시 보고서의 완료 1초 구간과 고정 흐름 frame/sequence가 별도로 기록되는 것을 확인했습니다. 동일 원본의 실제 실행속도나 모니터 scan-out을 측정한 결과는 아닙니다.

선택 보기 좌우/탭 버튼을 추가한 `A816079B0F2B355E2F16F5D2599C10E27FCC311E076E797A9BA64703C62A2EE4`는 [빌드](../../../build/live-root-review/selected-layout-build.txt)와 기존 [계약 6/6](../../../build/live-root-review/selected-layout-contracts.txt)이 통과했습니다. root의 실제 클릭 검증에서 Overview OFF / Flow·Controls ON 상태로 좌우 2열 도킹, 같은 영역의 두 탭 배치, Controls 탭 선택, 그 안에서 Vulkan Reuse false→true 적용, 다시 좌우 배치까지 통과했습니다. [보고서](../../../build/live-root-review/flow-layout-vulkan.txt), [실행 hash](../../../build/live-root-review/flow-layout-vulkan.json), [최종 캡처](../../../build/live-root-review/flow-layout-vulkan.png), [2,700 Present 후 exit 0](../../../build/live-root-review/flow-layout-vulkan-exit.json)을 보존했습니다.

이때 발견한 선택 보기의 최초 floating 위치가 헤더를 덮는 표시만 후속 보완했습니다. 실제 toolbar 끝 좌표 아래를 첫 위치로 지정하며 이후 이동·도킹 위치를 매 프레임 재설정하지 않습니다. 최종 Release GUI SHA256은 `A88B7EEFA917CF1E47D4FE60596C81748C98F662E868D65D68DE19E467700DCC`이며 [빌드](../../../build/live-root-review/optional-position-build.txt)가 성공했습니다. root가 이 바이너리에서 Controls가 헤더·배치 버튼을 가리지 않는 최초 위치, Flow·Controls의 동일 OS 창 좌우 2열 배치와 같은 영역 탭 배치를 실제 클릭으로 재확인했습니다. [최종 실행 hash](../../../build/live-root-review/flow-final-gui.json), [1,200 Present 보고서](../../../build/live-root-review/flow-final-gui.txt), [원본 캡처](../../../build/live-root-review/flow-final-gui.ppm), [GUI exit 0](../../../build/live-root-review/flow-final-gui-exit.json)을 보존했습니다. C 채팅은 해당 파일의 A88 hash·1,200 Present·exit 0을 대조했고 GUI를 직접 조작하지 않았습니다.

Sky 자동 drag에서 창이 이동하지 않은 결과만으로 앱 또는 backend 결함을 확정하지 않았으며 입력 backend를 수정하지 않았습니다. 상단 배치 버튼의 실제 도킹·탭 전환 PASS와 자동 drag의 미확정 상태를 구분합니다.

## 이전 GUI 실측 (2026-10-15, 표시 주기 개편 전)

이 절과 아래 GUI 자체 비용은 **폴더 런처 변경 전** `DB42...` 바이너리의 보존된 실측입니다. 새 런처 바이너리의 성능 재측정으로 해석하지 않습니다.

root가 Sky로 실제 checkbox/Apply 입력·창 전환·screenshot 검증을 수행했습니다. C 채팅은 아래 보고서를 대조했으며 GUI를 독립 조작하지 않았습니다. 검증한 Release GUI SHA256은 `DB42B26E316B477AEA69FEDC80A946598695C1E522EEB78422AE05227A188DD2`입니다.

| 분석 대상 | 실제 옵션 전환 | 표시 지연 p95 | 최소~최대 | 평균 | GUI Present |
|---|---:|---:|---:|---:|---:|
| D3D12 example14 | 20회, Applied rev 2~21 | 185.869 ms | 65.418~316.368 ms | 142.33615 ms | 10,791 |
| Vulkan example15 | 4회, Applied rev 2~5 | 142.252 ms | 111.112~142.252 ms | 130.27125 ms | 50,140 |

당시 D3D12의 20개 표본은 **당시 p95 ≤250 ms 목표를 충족**했습니다. 이 기준은 현재 1Hz/3초 동작의 판정 기준으로 사용하지 않습니다. 1회는 316.368 ms로 250 ms를 초과했습니다. Vulkan의 4개 표본은 기능·보조 지연 검증이며 충분한 표본의 p95 일반 보장으로 해석하지 않습니다. 두 실행 모두 QPC 일치와 GUI 정상 종료(exit 0)를 확인했습니다. 보고서의 지연값을 root가 독립 재계산한 [최종 요약](../../../build/live-root-review/gui-latency-summary-final.json)과 원본 [D3D12 보고서](../../../build/live-root-review/gui-fixed-d3d12-example14.txt), [Vulkan 보고서](../../../build/live-root-review/gui-fixed-vulkan-example15.txt)를 보존했습니다.

- D3D12: Instancing 전환에 따라 Draw·batch 1↔256, objects 256을 확인했습니다. 대상 창 전면 전환·최대화·복원·뷰어 복귀가 통과했습니다. 연결 해제 후 age 136→6,711 ms 증가, 재연결 후 59 ms 복귀를 확인했습니다. Expand IDs에서 Batch 23의 요소 256·8,192 bytes·EndOfInput·원래 frame 표시를 확인했습니다.
- Vulkan: create↔reuse 전환, 프레임당 upload 8,192 B 유지와 buffer ID 재사용/변경을 확인했습니다. GPU 탭의 Vulkan 1 queue·원래 frame Completed 0.015 ms·calibration error 2,816 ns 표시와 대상 창 최대화·복원·뷰어 복귀가 통과했습니다.

## GUI 자체 비용

동일한 수정 후 Release GUI를 연결한 기본 그룹 Flow에서 41.8651039초를 관측했습니다. GUI 1540×960, D3D12 example14 instanced 대상 800×600 FIFO, 배경 사용자 게임을 그대로 둔 조건입니다. GUI CPU 시간은 6.78125초로 논리 코어 하나 기준 16.1978578%, 22개 논리 코어 전체 용량 기준 약 0.736%였습니다. Working set은 177.836 MiB, private memory는 197.543 MiB였습니다.

| GUI 프로세스 GPU Engine 카운터 | 평균 | 최대 | 엔진별 표본 |
|---|---:|---:|---:|
| 3D | 0.4634456% | 0.5112983% | 15 |
| copy | 1.3614387% | 1.5343501% | 15 |
| graphics_1 | 0.8489737% | 1.0215835% | 15 |

이는 관측된 프로세스별 엔진 카운터이며 합계를 전체 GPU busy로 해석하지 않습니다. [원본 비용 자료](../../../build/live-root-review/gui-fixed-cost-d3d12-instanced.json)에 측정 조건·프로세스·엔진별 값을 보존했습니다. 기존 [18회 성능 비교 자료](../../../build/live-root-review/performance-summary.json)는 GLFW 패치 전 target의 `TargetSha256`과 측정 환경을 보존한 이력입니다. 해당 파일의 `GuiStatus`는 그 실험 당시 상태이며, 최신 GUI 검증은 위 Release GUI 해시의 실측 결과를 따릅니다.

원본 공통 계약: [`SharedContracts.md`](../../../docs/execution-visualizer/SharedContracts.md).
