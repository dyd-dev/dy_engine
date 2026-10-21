# Performance Playground 구현 계획

## 2026-10-21 main API 호환성 작업

현재 목표는 `main`의 공개 API만으로 게임을 빌드하고 검증한 뒤 `feature/performance-playground`에 커밋·푸시하는 것이다. 아래 2026-10-16 기록의 브랜치/커밋 제한은 당시 작업 범위다.

- [x] 기존 소스의 MSVC 구문 검사와 전체 빌드로 공개 API 변경 및 셰이더 실패를 확인한다.
- [x] `Controls.h`와 `CompatibilityTests.cpp`에서 여러 설정을 같은 프레임에 요청해도 앞선 요청을 유지하고, 실제 렌더링 후 적용되는지 검증한다.
- [x] `main.cpp`는 `Renderer::SetConfig()`와 장치 소유 `recordingThreadCount`를 사용한다. `World.cpp`의 장면 접근을 현재 공개 형식에 맞춘다.
- [x] `Evidence.h`는 공개 `ProfilerTimingSnapshot`을 기록한다. 미완료/미지원/비활성 GPU 값은 null이며 비공개 source/frame/revision 정보는 Unavailable로 명시한다. 호환성 테스트에서 상태별 직렬화를 검사한다.
- [x] CMake/Tracy 연결을 `dyf::Tracy`와 명시적 SDK 의존성으로 갱신하고, 저장소에 없는 이전 RenderGraph 테스트 의존성을 제거한다.
- [x] D3D12 Release 빌드, 모든 게임 CTest, 실제 게임의 제한 프레임 실행 및 Tracy 옵션 빌드를 확인한다. 현재 검증 범위와 역사적 결과를 문서에서 구분한다.
- [x] 공통 HLSL의 두 오류만 수정한다: `mesh_ps.hlsl`의 예약어 `point` 지역변수 이름, `mesh_shadow_ps.hlsl`의 VS/PS 입력 레지스터 불일치. 조명+그림자 동시 렌더링 회귀 검사를 통과시킨다.

게시 절차: 에셋 해시, 변경 범위, 리뷰 결과를 확인한 후 게임 폴더와 두 셰이더 수정만 커밋·푸시한다. 라이선스 원본 바이트는 `.gitattributes`로 보존하며 기존 main, stash, 제외 설정은 보존한다.

검토 초점: 같은 프레임의 연속 설정 요청, 요청과 적용 상태 구분, 미완료 GPU 값의 0 대체 방지, 다운로드 캐시 없는 빌드 의존성, 비공개 엔진 헤더에 대한 우회 의존성.

## 2026-10-16 초기 구현 기록

목표: dy_engine Windows 네이티브 게임에서 외부 무료 3D 에셋으로 만든 네 구역을 실제 조작하고 기존 Execution Visualizer로 관측한다.

범위와 구조: 이 폴더의 독립 CMake 프로젝트가 저장소 엔진을 하위 프로젝트로 사용한다. 기존 01~28 예제, ExecutionVisualizer UI, 공유 헤더, 테스트 제외 설정은 바꾸지 않는다. 커밋/푸시/브랜치 이동은 하지 않는다. 작업은 이 채팅에서 직접 진행한다.

- [x] 공식 Kenney CC0 패키지 다운로드, 라이선스 원문·링크·SHA256과 실제 사용 목록 보관.
- [x] `ControllerTests.cpp`로 대각선 속도, 달리기, 벽 충돌/미끄러짐, 문 통과, 큰 프레임 간격, 경계와 카메라 충돌을 먼저 검증. `Controller.h`에 지면 이동과 3인칭 카메라 구현.
- [x] `World.cpp/.h`에서 기준 광장, 밀집 외벽, 문/창/복도를 가진 실내, 그림자 마당을 연결. 실제 외부 건물/캐릭터/부착물/소품 사용. 재사용 메시/재질은 엔진 인스턴스 경로에 연결.
- [x] `main.cpp`에서 실제 Window/Input/Camera/Renderer/Canvas 재사용. WASD/방향키, Shift 달리기, 마우스 시점, 흔들림, 고정 비교 위치, 실제 제어 토글, 화면 조작 안내와 명시적 GPU 상태 표시.
- [x] 독립 Release 빌드와 CTest, 네이티브 GPU 실행, 실제 키보드/마우스 이동·달리기·문 통과와 각 구역 화면 확인. 실행 파일/에셋/관측 증거 보관.
- [x] 기존 분석기 연결과 실제 draw/upload/GPU 상태 확인. 관측 위치·설정 리비전 구분. 가림을 occlusion culling이나 GPU culling으로 표현하지 않음.
- [x] 최종 diff/범위 검토, 재현 가능한 실행 안내와 검증/한계/전체 생성 파일 목록 기록.

검토 초점: 포커스 상실 시 입력, 얇은 벽 통과 방지, 실내 카메라가 벽 밖으로 넘어가는 문제, 실행 작업 폴더가 달라도 에셋 탐색, GPU 지연/미지원 값을 0으로 표시하는 오류. 데이터 파일만 다운로드하며 외부 프로그램은 설치하지 않는다.

2026-10-16 초기 상태: execution-visualizer 브랜치, 기존 공통/RHI/UI 미커밋 변경 존재. stash 9개, 현재 tracked tests skip-worktree 66개(위임문에 적힌 75개와 실제 조회 결과가 다름). 해당 상태는 변경하지 않는다.

완료 증거는 VERIFICATION.md에 있다. 도중 애니메이션 전용 FBX는 기존 Model 로더가 geometry 부재로 거부하는 것을 확인했고, 공통 엔진을 수정하는 대신 같은 ufbx 의존성으로 이 게임 폴더 안에서 읽었다. FBX 로컬 좌표 대신 전체 bind 좌표를 기준으로 캐릭터 크기를 보정했다. 외부 코드/에셋 의존성을 새로 설치하지 않았다.
