# Performance Playground / Foundry District

dy_engine의 Windows D3D12/Vulkan 네이티브 게임이다. 3인칭 캐릭터로 연결된 도시 구역을 돌아다니며 기본 Scene Renderer의 실제 렌더링 비용과 설정을 비교한다. 현재 `main`의 공개 API를 사용한다.

## 실행

Build.ps1의 기본 출력: `../../../build/performance-playground-native/Release/PerformancePlayground.exe`.
실행 파일 옆 `Assets`를 함께 유지한다. 실행 작업 폴더가 달라도 EXE 위치에서 에셋을 찾는다. Windows 기본 Segoe UI 글꼴을 읽으며 글꼴 파일은 재배포하지 않는다.

```powershell
& ./examples/games/PerformancePlayground/Build.ps1
& ./build/performance-playground-native/Release/PerformancePlayground.exe
```

Visual Studio 2022 C++, Windows SDK, CMake, DXC가 필요하다. Build.ps1은 이 체크아웃에 이미 있는 의존성 소스가 있으면 재사용한다. 없는 의존성은 엔진의 기존 FetchContent 설정으로 받는다. 에셋은 저장소에 포함되어 있다. 다시 받으려면 `FetchAssets.ps1`을 실행한다(네트워크 필요, 원본 ZIP SHA256 확인). 전체 게임 CTest에는 컨트롤러, 실제 장치에서의 설정 적용/렌더링, 에셋 로드 검사가 포함된다.

## 조작

| 입력 | 동작 |
|---|---|
| WASD / 방향키 | 지면에서 전후좌우 이동 |
| Shift | 달리기(걷기 3 m/s, 달리기 6 m/s) |
| 마우스 왼쪽/오른쪽 드래그 | 시점 회전 |
| 휠 / Tab | 카메라 거리 / 마우스 잠금 전환 |
| Esc | 잠긴 마우스 해제, 해제 상태에서는 종료 |
| B / R | 이동·달리기 카메라 흔들림 / 자동 걷기 전환 |
| 1 / 2 / 3 / 4 | 기준 광장 / 밀집 골목 / 실내 / 빛과 그림자 비교 위치 복원 |
| 5 | 건물 출입구 앞(걸어서 실내 진입 가능) |
| I / C | 기본 Scene 인스턴싱 / CPU 프러스텀 컬링 제어 요청 |
| L / H | 조명 / 그림자 제어 요청 |
| P / V | RenderGraph 병렬 기록 / VSync 제어 요청 |
| F9 / F10 | EXE 옆 Captures에 화면 PPM + 원본 계측 JSON / 계측 JSON 저장 |

키 토글은 `Renderer::GetConfig()`의 미적용 요청을 유지하면서 `SetConfig()`로 다음 렌더링에 적용할 설정을 요청한다. HUD는 `GetAppliedConfig()`의 실제 적용 상태를 읽는다. 같은 프레임에 여러 키를 눌러도 앞선 요청을 덮어쓰지 않는다. 게임 창이 포커스를 잃으면 자동 걷기가 멈추고 마우스 잠금이 풀린다.

## 비교 구역

1. **기준 광장**: 넓은 바닥과 소수의 가까운 건물. 중앙 도로가 다른 구역으로 연결된다.
2. **밀집 골목**: 기존 건물에 파이프·배수관·차양·외벽 장식을 800개 배치했다. 반복되는 메시/재질은 엔진의 인스턴싱 경로로 제출된다.
3. **실내**: 실제 열린 문, 창, 복도 개구부, 천장과 벽 충돌이 있다. 5번 위치에서 앞으로 걸으면 출입구를 통과한다. 실내에서 일부만 보이는 외부 장면을 관찰한다.
4. **빛과 그림자**: 기둥과 보, 컨테이너, 굴뚝, 물탑 및 산업 건물. 방향광과 색이 다른 두 스폿 광원이 실제 shadow pass를 사용한다. 실내 점광원은 그림자를 만들지 않는다.

## 계측과 설정

HUD와 F10 JSON은 공개 `Renderer::GetProfilerTimingSnapshot()`을 사용한다. CPU/프레임 평균과 완료된 GPU frame/main/shadow 표본, 집계 구간의 길이와 공개 순번을 기록한다. GPU 표본이 없으면 `Pending`, 장치가 계측을 지원하지 않으면 `Unsupported`, 그림자가 비활성이면 `Unavailable`이며 수치는 `null`이다. 실제로 측정된 0은 `Ready`와 함께 기록한다.

`timing.sequence`는 약 200 ms 간격으로 발행되는 집계 스냅샷의 순번이다. 현재 프레임 ID나 설정 revision이 아니다. draw/instance 수와 적용 설정은 최근 렌더링의 값이며, 집계 시간 구간과 일대일로 대응하지 않는다. 설정을 바꾼 직후의 구간은 성능 비교에서 제외한다.

`main`은 이전 `dyf::Diagnostics`와 외부 Execution Visualizer API를 공개하지 않는다. 개별 frame/device/source/revision, 상세 GPU span, 원격 제어/손실 카운터는 현재 게임에서 제공하지 않으며 JSON의 `liveDiagnostics.state`는 `Unavailable`이다. 내부 헤더를 가져와 이 경계를 우회하지 않는다. 기존 분석기 연결 증거는 이전 브랜치의 역사적 기록이다.

같은 비교 위치로 복원하고 캐릭터를 멈춘 뒤 하나의 옵션만 바꿔 관측한다. 프레임 주기에는 데스크톱 응답성을 위한 최대 8 ms의 대기와 다른 실행 중인 앱의 영향이 포함된다. CPU render/GPU main/shadow는 각 범위의 실제 엔진 계측이다. 그림자/조명 토글은 출력 품질도 바꾼다. F9는 readback과 저장 비용을 발생시키므로 그 프레임을 정상 성능 표본으로 사용하지 않는다.

**벽에 가려지는 것과 draw 제거는 다르다.** 이 기본 Scene 경로에는 occlusion culling/GPU culling이 없다. 보이지 않는 물체도 프러스텀 안이면 제출될 수 있다. HUD의 culled는 CPU 프러스텀 결과이며 main draw/instance 수에는 그림자와 HUD가 포함되지 않는다. GPU 수치가 준비되지 않으면 0으로 표시하지 않는다.

## Tracy 선택 빌드

`DY_ENABLE_TRACY=ON`으로 구성하면 공개 `dyf::Tracy`와 `TracyAdapter`를 연결한다. 게임 힙 계측은 Tracy SDK에 명시적으로 연결한다. 기본 빌드는 Tracy를 끄며, Vulkan은 CMake에 `USE_VULKAN=ON`, `USE_D3D12=OFF`를 지정한다.

- 현재 Tracy 어댑터는 CPU 구간, 프레임 표식, 준비된 GPU 평균값과 리소스 수를 전달한다. 이전 브랜치의 Vulkan 큐 GPU 타임라인과 작업 큐 락/엔진 호출 스택을 재현한다고 보장하지 않는다.
- `Memory`에서 **Memory pool → Game C++ heap**을 선택한다. 실행 파일에 연결된 C++ `new/delete`의 할당·해제·활성 할당과 할당 호출 스택을 보여 준다. `Default allocator`는 비어 있을 수 있다. DLL 내부 malloc, 프로세스 전체 메모리와 VRAM은 포함하지 않는다. 연결 이전부터 살아 있던 할당도 전체 힙 사용량으로 해석하지 않는다.
- **P**는 장치가 소유하는 작업자 두 개로 병렬 기록을 요청한다. 게임은 ThreadPool 객체를 소유하거나 내부 작업자 헤더를 포함하지 않는다.
- 게임 C++ 힙 이벤트는 최대 8단계 호출 스택을 수집한다. MSVC Tracy 빌드에서는 게임 PDB를 생성하므로 EXE와 함께 보관한다.

전역 할당 호출 스택 계측은 이벤트 양과 CPU 비용을 크게 늘릴 수 있다. 짧은 캡처로 조사하고 성능 비교용 기본 빌드는 Tracy를 끈다.

Tracy 빌드는 추가 allocator CTest를 제공한다. 과거 분석기와 연결한 상세 캡처 결과는 [TRACY_VERIFICATION.md](TRACY_VERIFICATION.md)에 보존되어 있으며 현재 브랜치의 검증 결과와 구분한다.

## 표현과 검증의 한계

캐릭터는 Kenney의 외부 FBX 메시/스킨과 idle/run 키프레임을 사용한다. 관절별 강체 삼각형 분할로 표현하여 기본 Scene 인스턴싱/컬링 경로를 유지한다. 부드러운 GPU 스키닝의 성능 비교는 이 게임의 측정 대상이 아니다. 메시가 없는 애니메이션 FBX는 엔진에 이미 포함된 ufbx로 제한된 크기에서 읽고 dyf의 키프레임 샘플링/계층 계산을 사용한다.

이 데모는 평지용 원형 플레이어 충돌과 건물 AABB/수동 실내 개구부를 사용한다. 계단·점프·경사·물리 시뮬레이션은 구현하지 않는다. 비교 위치를 바꾸면 포즈가 복원되지만 애니메이션 시계와 GPU 실행 상태까지 결정론적으로 고정되지는 않는다.

에셋 출처·라이선스·원본 ZIP/파일 해시는 `ASSET_SOURCES.md`, `Assets/manifest.json`, 각 패키지의 `License.txt`에 있다. 현재 main 호환성 검증은 [MAIN_API_VERIFICATION.md](MAIN_API_VERIFICATION.md), 초기 구현의 역사적 증거는 [VERIFICATION.md](VERIFICATION.md)에 있다.
