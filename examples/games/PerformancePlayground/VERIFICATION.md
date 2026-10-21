# 구현 및 검증 결과

이 문서는 `execution-visualizer` 기반의 2026-10-16 실행 기록이다. 현재 main API 빌드의 증거와 한계는 [MAIN_API_VERIFICATION.md](MAIN_API_VERIFICATION.md)를 참조한다. 아래 실행 파일 해시·분석기 연결 결과는 현재 브랜치에 그대로 적용되지 않는다.

검증일: 2026-10-16 (Asia/Seoul). 대상: 이 PC의 Windows 네이티브 D3D12 기본 어댑터.

## 결과와 실행 파일

`build/performance-playground-native/Release/PerformancePlayground.exe`와 같은 폴더의 `Assets`가 실행 결과물이다. EXE SHA256:

`86545710956B89101F716DC4C55B1F74B368B7CC7F830A2FB2C4E18D9AF806AA`

엔진 공통 소스는 이 실행 파일보다 뒤에 변경된 것이 없음을 최종 조회로 확인했다(별도 진행 중인 ExecutionVisualizer UI 제외). 에셋은 공식 Kenney CC0 4개 ZIP에서 내려받았다. 선택 파일 42개와 manifest 1개를 보관하며, 게임은 그중 GLB 모델 25종, 캐릭터 FBX, 스킨, idle/run 애니메이션을 실제 사용한다. 장면은 **1,079개 엔티티 / 외벽 부착물 800개 / 충돌 AABB 76개**이다.

## 증거 수준

- **컨트롤러 검사:** 걷기 3 m/s, 달리기 6 m/s, 대각선 정규화, 얇은 벽/미끄러짐, 문 통과, 큰/음수 dt, 월드 경계와 3D 카메라 충돌. 누락된 Controller.h로 최초 실패를 확인한 후 구현했고 최종 통과했다. 초기 문 테스트의 벽 좌표 오타도 실제 실패로 확인하여 고쳤다.
- **게임 에셋/월드 검사:** 실제 사용 모델/텍스처/뼈대/클립 로드, 비교 지점의 통행 가능 여부, 실제 월드의 출입구를 150스텝 걸어 통과, 변환 후 캐릭터가 지면 위 1.5~2 m 범위인지 확인한다. 최종 CTest **2/2 통과**.
- **네이티브 GPU 검사:** `PerformancePlayground.exe --frames 120 --station 3 --validation`이 D3D12 검증 레이어를 켠 상태에서 **120프레임, exit 0**. `build/performance-playground-native/validation.log`에 D3D12 ERROR/CORRUPTION과 게임 렌더 오류가 없었다.
- **실제 입력:** Windows Computer Use로 최종 Release 창에 W/A/S/D, Shift+W, 마우스 드래그, 숫자키와 R을 보냈다. 실제 캐릭터 위치/애니메이션/카메라/그림자와 HUD를 화면으로 확인했다. 5번 지점 `(-18,6.5)`에서 R 자동 걷기를 시작해 문을 통과했고, 실내 뒤쪽 벽 `y≈26.5`에서 정지하는 것도 확인했다. 실내에서 반대로 걸어 `(-18.0,1.6)` 실외로 나왔다. 자동 걷기도 키보드로 시작/정지했으며 스크립트가 위치를 직접 이동시킨 검사가 아니다. 숫자키 위치 복원과 문을 걸어 통과한 검사는 구분한다.

실제 입력 파일 `Evidence/ReleaseFinal/station-3-6.json`:

| 항목 | 기록 |
|---|---:|
| 이동 키 마스크 (W/A/S/D) | 15 (모두 입력됨) |
| 누적 걷기 | 20.27648 m (자동 걷기 포함) |
| 누적 달리기 | 0.11988 m (실제 Shift+W 입력) |
| 마우스 수평 회전 | 3.14000 rad |
| 최대 관측 카메라 흔들림 | 0.06391 m |
| 걸어서 문 통과 | true |

위 기록은 게임 내부 이동 결과이며 CPU/GPU 성능 지표가 아니다. 전체 장시간 플레이나 다른 PC/입력 장치 검증을 대신하지 않는다.

## 실제 렌더 비교

다음은 같은 게임 세션의 실제 main-pass 제출량이다. main 카운터는 그림자와 Canvas/HUD를 제외한다. F9 캡처 시에는 readback이 발생하므로 캡처 프레임의 시간 수치로 개선율을 계산하지 않는다.

| 위치/설정 | main draws | instances | frustum culled | 증거 파일 |
|---|---:|---:|---:|---|
| 기준 광장 | 41 | 41 | 1,038 | station-1-1.json / .png |
| 밀집 골목, 인스턴싱 ON | 103 | 720 | 359 | station-2-2.json / .png |
| 같은 골목, 인스턴싱 OFF | 720 | 720 | 359 | station-2-3.json |
| 같은 골목, 인스턴싱 ON / 컬링 OFF | 199 | 1,079 | 0 | station-2-4.json |
| 실내에서 문 너머 외부를 보는 시점 | 93 | 108 | 971 | station-3-5.json / .png |
| 빛/그림자 구역 | 60 | 73 | 1,006 | station-4-8.json / .png |

I/C/L/H/P를 통해 등록된 Live Control의 적용 결과가 실제 바뀌는 것을 확인했다. `station-4-9`는 그림자 OFF, `station-4-10`은 조명 OFF, `station-4-11.json`은 병렬 기록 ON이다. 이들은 출력 품질/기록 경로 변경의 증거이며 측정된 최적화 개선율 주장이 아니다. 일부 최상위 통계와 계측 스냅샷은 서로 다른 시점에 읽힌다. JSON `sources` 내부 원본 frame/device/source/revision이 각 live 샘플의 식별자이고, GPU span은 자신의 원본 프레임을 별도로 가진다. 최신 프레임의 GPU가 Pending인 동안 과거 완료된 span이 Ready인 것은 정상이다.

## Execution Visualizer 연결

기존 `build/auto-observe-viewer/Release/dy_execution_visualizer.exe`를 최종 게임 **PID 5288**에 `--attach-pid`로 연결했다. 별도 UI 소스는 수정하지 않았다. 분석기 실행 파일 SHA256:

`13C9C128569A36794FA8644A519A269B5C003A5A8906405BC0F02F3FD3C506CF`

자체 보고서 `Evidence/ReleaseFinal/viewer-report.txt`와 실제 렌더 캡처 `viewer.png`에서 확인한 내용:

- 네이티브 분석기 렌더 360프레임, 수신 스냅샷 127개, client_state=2 (Connected), QPC 원점 일치.
- device=1081, source=1084, received frame=109675598, revision=22.
- 원본 draw=21 / indexed draw=754, upload bytes=142592.
- 완료된 1초 GPU measured 평균 3.983863 ms가 Ready로 전달됨. 통계/Flow가 실제 화면에 표시됐고 Controls에 등록된 적용 상태도 전달됨.
- 병렬 기록 false→true의 적용 리비전 21→22와 이전/이후 원본 구간을 분석기가 수신함. 부하 동일성이 확인된 개선율이라고 주장하지 않음.

분석기 창의 Computer Use 조회는 앱 승인 시간이 초과되어 추가 UI 입력을 하지 않았다. 위 UI 확인은 분석기가 자체 저장한 네이티브 렌더 캡처를 열어 검토한 결과이며, 분석기 버튼을 직접 눌러 조작한 검증은 포함하지 않는다. 보고서 원본 `viewer.json`은 기존 분석기의 key=value 텍스트 출력이며, 읽기 편하게 동일 내용의 `viewer-report.txt`도 남겼다.

## 제한과 보존 사항

- 기본 Scene 경로에는 **occlusion culling/GPU culling이 없다**. 벽/창에 화면상 가려져도 프러스텀 안의 draw는 남을 수 있다. 실내의 culled 값은 CPU 프러스텀 판정이다.
- 캐릭터는 외부 idle/run 키를 사용한 관절별 강체 삼각형 표현이다. 관절 경계가 보일 수 있고 부드러운 GPU 스키닝 성능 비교는 하지 않았다.
- 장시간 높은 명령량에서는 기존 collector의 제한 때문에 일부 세부 이벤트가 생략됐다. 분석기 화면에도 이를 명시한다. 저장한 보고서에서 frame loss와 GPU span loss는 0이며, 전체 명령 흐름의 모든 세부가 보존됐다고 주장하지 않는다.
- GPU 값은 원래 Ready/Pending/Unsupported/Unavailable를 보존한다. 없는 GPU 쿼리/occlusion 결과를 만들지 않았다. 그림자 OFF는 실제 그림자 pass를 제거하는 출력 변경이다.
- 평지 이동과 AABB 충돌만 구현했다. 점프/경사/계단, 부드러운 GPU 스키닝 비교, Vulkan/Metal과 다른 PC의 실행은 검증하지 않았다. 다른 GPU 앱이 실행 중인 공유 데스크톱이므로 재현 가능한 성능 개선율의 근거로 사용하지 않는다.
- 변경은 `examples/games/PerformancePlayground/`의 신규 파일과 전용 `build/performance-playground-*` 출력뿐이다. 기존 01~28 예제, ExecutionVisualizer UI, 공통 엔진 파일은 이 작업에서 수정하지 않았다. 커밋/푸시/병합/브랜치 변경 없음. stash 9개, tracked tests skip-worktree 66개와 `.git/info/exclude`의 `tests/` 규칙을 유지했다.

화면 PNG는 원본 PPM을 Pillow로 무손실 변환하고 픽셀 바이트 일치를 확인했다. 원본 PPM도 보존했다. 생성 파일 전체 목록은 FILES.md와 Assets/manifest.json에 있다.
