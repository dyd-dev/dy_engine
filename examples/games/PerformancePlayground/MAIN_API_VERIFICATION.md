# main API 호환성 검증

검증일: 2026-10-21 (Asia/Seoul). 기준 main: `5c3bb9d7527d90b2916ff7694ace5fcc6c65ac6a`. 대상: Windows x64, Visual Studio 2022/MSVC 19.44, D3D12 기본 어댑터.

## 변경과 영향

- 게임 설정은 공개 `Renderer::GetConfig()/SetConfig()`로 요청하고 HUD/JSON은 `GetAppliedConfig()`를 읽는다. 같은 프레임의 복수 요청을 보존한다.
- 병렬 기록 작업자는 장치가 소유하며 `recordingThreadCount=2`를 사용한다. 장면의 변환은 현재 `float4x4` 반환값을 직접 사용한다.
- JSON은 공개 `ProfilerTimingSnapshot`의 집계 순번·기간·CPU/GPU 값만 기록한다. 개별 프레임의 source/revision, 상세 span, 원격 제어 상태는 공개되지 않아 `liveDiagnostics.state=Unavailable`이다. 없는 GPU 값은 null이며 측정된 0과 구분한다.
- Tracy 선택 빌드는 공개 `TracyAdapter`, `dyf::Tracy`, 명시적 SDK 링크와 게임 힙 계측을 사용한다. 게임 EXE의 PDB도 생성한다.
- 공통 HLSL은 두 지점만 수정했다. `mesh_ps.hlsl`의 예약어 `point` 지역변수를 `clippedVertex`로 변경하고, `mesh_shadow_ps.hlsl` 입력에 `SV_POSITION`을 넣어 그림자 VS/PS의 TEXCOORD 레지스터를 맞췄다.
- 외부 에셋 42개는 변경하지 않았다. 원본 License.txt의 줄바꿈이 Git에서 변환되어 manifest 해시가 달라지지 않도록 `Assets/.gitattributes`에 `-text`를 지정했다.

## 실행한 검증

| 검사 | 결과 |
|---|---|
| 수정 전 MSVC 구문 검사 | 비공개 Diagnostics/GetLiveSourceId와 제거된 ThreadPool 헤더로 실패 재현 |
| 수정 전 전체 D3D12 빌드 | HLSL 예약어 `point` 오류 재현 |
| 수정 전 조명+그림자 렌더 회귀 검사 | 실제 PSO 생성 실패 재현; Debug 메시지 660으로 VS/PS TEXCOORD register 불일치 확인 |
| D3D12 Release 전체 빌드 | 통과 |
| 기본 게임 CTest | PlaygroundController, PlaygroundCompatibility, PlaygroundAssets **3/3 통과** |
| Tracy ON Release 전체 빌드 | 통과, EXE/PDB 생성 |
| Tracy 게임 CTest | 위 세 검사와 PlaygroundTracyMemory **4/4 통과** |
| 실제 Release 게임 | 네 비교 구역 각각 120프레임 정상 종료, 1,079 entities / 800 attachments / 76 colliders |
| 계측 JSON | 네 구역 모두 유효한 JSON, GPU frame/main/shadow Ready 값 확인; 미측정 상태/null 회귀 검사 통과 |
| Tracy ON 실제 게임 | 기준 광장 60프레임 정상 종료 |
| Debug + D3D12 검증 레이어 | 빛/그림자 구역 30프레임 정상 종료, severity 0/1 오류 0개 |

Debug 로그에는 버퍼 초기 COPY_DEST 상태를 COMMON으로 처리한다는 메시지 1328(160회)과 깊이 텍스처 생성 시 최적화용 clear value가 지정되지 않았다는 메시지 821(60회)이 남는다. 후자는 clear 동작은 수행되지만 느릴 수 있다는 경고다. 이 실행을 무경고 실행으로 표현하지 않는다.

`PlaygroundCompatibility`는 실제 Renderer에서 요청/적용 상태, 같은 프레임의 두 설정 요청, 여섯 토글, readback, 병렬 기록, 조명과 그림자의 동시 렌더링을 검사한다. 계측 직렬화는 Pending/Unsupported/Unavailable의 null, 미완료 표본의 stale 값 제외, Ready인 0과 1.25 ms 보존을 검사한다.

## 재현과 로컬 결과물

```powershell
& ./examples/games/PerformancePlayground/Build.ps1 -BuildDirectory ./build/performance-playground-main
ctest --test-dir ./build/performance-playground-main -C Release --output-on-failure
& ./build/performance-playground-main/Release/PerformancePlayground.exe --frames 120 --station 1 --validation --capture-dir ./build/playground-check
```

Tracy는 별도 CMake 빌드 디렉터리에서 `DY_ENABLE_TRACY=ON`, 기본 게임은 OFF를 사용했다. 양쪽 모두 현재 main 기본값인 `DY_ENABLE_LIVE_PROFILING=OFF`로 검증했다. GLFW/stb/fastgltf/ufbx/bcdec/meshoptimizer/Tracy는 이미 내려받은 소스를 재사용했고 새 패키지를 추가하지 않았다.

- 기본 EXE: `build/performance-playground-main/Release/PerformancePlayground.exe`
- Tracy EXE/PDB: `build/performance-playground-main-tracy/Release/PerformancePlayground.*`
- 빌드/실패 재현/Debug 로그: `build/performance-playground-commit-prep-20261021/`
- 구역별 session.json: 같은 폴더의 `native-final/station-1`부터 `station-4`
- 이전 브랜치의 캡처 기록: [VERIFICATION.md](VERIFICATION.md), [TRACY_VERIFICATION.md](TRACY_VERIFICATION.md)

검증한 Release EXE SHA256:

- 기본: `83D5AA788CE174AC889274C318E912B1368C1CE4646E3F09BECA1C4880FD087E`
- Tracy: `053190DF28590EE13000D568009DA4698980B93AD2EF63DD5FDE41BDC29B36D1`

이번 검증은 전체 게임 빌드·자동 테스트·네이티브 실행이다. 실제 키보드/마우스 재조작, F9 이미지의 육안 비교, Tracy GUI 연결/캡처, Vulkan/Metal 실행은 다시 검사하지 않았다. 구역별 GPU 수치는 빌드/다른 앱 부하가 있는 환경의 동작 증거이며 성능 비교 결론으로 사용하지 않는다.
