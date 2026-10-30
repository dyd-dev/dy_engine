# Execution Visualizer 소스 보관

커밋 `0cabbc6`의 Windows용 ImGui 분석기 소스를 보관한다. 현재 빌드에는 포함하지 않으며, 이전 CMake와 테스트는 복원하지 않았다.

기존 Public `Live*` API가 이름을 바꾸고 비공개 영역으로 이동했으므로 이 소스는 아직 현재 엔진에 맞춰 이식하거나 빌드할 수 있는 상태가 아니다.

| 파일 | 역할 |
|---|---|
| `FlowGraph.*`, `FlowPlayback.*`, `FlowView.cpp` | 수집한 실행 흐름의 그래프 구성, 순차 재생과 화면 표시 |
| `ControlsView.cpp`, `ComparisonView.cpp` | 대상의 제어값 변경과 변경 전후 측정값 비교 |
| `Viewer.*`, `ViewerState.*`, `main.cpp` | 분석기 화면, 연결·선택·표시 상태와 진입점 |
| `LaunchOptions.*`, `LaunchView.cpp`, `Process.*` | 대상 실행 인자 편집, 실행 화면과 Windows 프로세스 관리 |

현재 엔진의 대응 코드는 [`src/Platform/Profiler`](../src/Platform/Profiler)에 있다. 수집기 내부 계약은 [`CollectorInternal.h`](../src/Platform/Profiler/CollectorInternal.h), 연결 계약은 [`Connection/Protocol.h`](../src/Platform/Profiler/Connection/Protocol.h), 클라이언트 구현은 [`Connection/Client.cpp`](../src/Platform/Profiler/Connection/Client.cpp)를 참고한다.
