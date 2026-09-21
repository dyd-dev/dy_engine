# 다중 스레드 명령 기록

독립된 두 텍스처의 입력을 만들고 업로드 명령을 작업 스레드에서 기록한다. 두 패스를 읽는 후속 패스는 작업 합류 뒤 기록한다. 컴파일된 초기/최종 상태와 제출 순서는 단일 스레드 경로와 같다.

```text
AdvancedMultithreadRecording --mode parallel --workers 2 --frames 3 --capture parallel.ppm
AdvancedMultithreadRecording --mode serial --frames 3 --capture serial.ppm
```

두 모드의 결과는 동일하다. 네이티브 백엔드는 제출 완료 펜스를 기다린 후 모든 업로드 바이트를 GPU 읽기 결과와 비교한다. `--capture`는 두 텍스처를 가로로 연결한 PPM을 저장한다. 창이나 스왑체인은 필요하지 않다. Null 백엔드는 명령·상태·펜스 계약을 검증하며 픽셀 읽기와 캡처는 제공하지 않는다.

`RenderGraph::ExecuteParallel(device, pool, lists)`는 비어 있는 출력 벡터에 닫힌 명령 목록을 반환한다. 목록을 반환 순서대로 한 번에 `Submit`하고 `DestroyCommandList`로 해제한다. `SubmitDesc`의 완료 펜스와 GPU 자원 보유 규칙은 기존 RHI와 같다. 그래프는 명령을 제출하거나 프레임을 시작/종료하지 않는다.

callback 공유 데이터는 호출자가 동기화해야 한다. 그래프를 실행하는 동안 그래프·프레임·스왑체인을 변경하거나 같은 명령 목록을 다른 스레드에서 사용하면 안 된다. 첫 callback 전에 import와 pipeline 참조를 보유한다. callback 실패 또는 예외가 있으면 이미 시작한 작업을 모두 합류한 뒤 목록을 폐기한다. 예외는 호출 스레드로 전달된다. `nullptr` pool, 작업자 1개, 같은 pool 작업자 안에서의 중첩 호출은 현재 스레드에서 기록한다.

`GlobalBarrier()`는 자원을 선언하지 않은 패스에서도 등록 전후 모든 패스 사이에 실행 경계를 만들고, 공통 RHI의 전역 메모리 배리어를 기록한다. 같은 장치 큐에서 사용하는 자원은 그래프에 import하지 않았어도 포함된다. 상태와 레이아웃 전환, 다른 큐 및 CPU와의 동기화는 별도다. D3D12는 전역 UAV 배리어, Vulkan은 모든 명령 범위의 메모리 배리어, Metal은 encoder 경계와 같은 큐의 tracked hazard로 번역한다.

`ExecuteParallel`은 callback 기록 후 `IDevice::PrepareCommandLists`로 네이티브 명령까지 작업 스레드에서 기록한다. 공통 RHI가 독립된 명령 저장 공간을 할당하고 작업을 합류하며, `Submit`은 상태를 검증한 뒤 준비된 목록을 순서대로 제출한다. 예제는 네이티브 기록을 포함한 CPU 기록 시간과 CPU 제출 시간을 분리해 표시한다. GPU 실행 시간이나 속도 향상을 보장하는 수치는 아니다. `--workers 0`은 하드웨어 작업자 수를 선택하고 `1`은 직렬 폴백을 사용한다.

`RendererConfig::enableParallelRenderGraph = true`와 호출자가 소유한 `threadPool`을 설정하면 Renderer의 Shadow, MainForward, ToneMap, Overlay와 HUD도 이 경로를 사용한다. 설정을 끄거나 pool을 생략하면 같은 그래프를 직렬 기록한다. 그래프가 렌더 타깃의 상태 전환과 제출 순서를 관리한다.

`-DDY_BUILD_TESTS=ON`으로 `RenderGraphTests`를 빌드하면 작업자 실제 중첩, 무자원 전역 경계, 70개 독립 목록, 초기/최종 상태, GPU 업로드, callback 예외·기록 실패 복구, 소유 핸들 해제 후 참조 유지, 펜스 완료를 검증한다.
