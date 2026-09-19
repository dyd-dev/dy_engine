# 다중 스레드 명령 기록

독립된 두 텍스처의 입력을 만들고 업로드 명령을 작업 스레드에서 기록한다. 두 패스를 읽는 후속 패스는 작업 합류 뒤 기록한다. 컴파일된 초기/최종 상태와 제출 순서는 단일 스레드 경로와 같다.

```text
AdvancedMultithreadRecording --mode parallel --workers 2 --frames 3 --capture parallel.ppm
AdvancedMultithreadRecording --mode serial --frames 3 --capture serial.ppm
```

두 모드의 결과는 동일하다. 네이티브 백엔드는 제출 완료 펜스를 기다린 후 모든 업로드 바이트를 GPU 읽기 결과와 비교한다. `--capture`는 두 텍스처를 가로로 연결한 PPM을 저장한다. 창이나 스왑체인은 필요하지 않다. Null 백엔드는 명령·상태·펜스 계약을 검증하며 픽셀 읽기와 캡처는 제공하지 않는다.

`RenderGraph::ExecuteParallel(device, pool, lists)`는 비어 있는 출력 벡터에 닫힌 명령 목록을 반환한다. 목록을 반환 순서대로 한 번에 `Submit`하고 `DestroyCommandList`로 해제한다. `SubmitDesc`의 완료 펜스와 GPU 자원 보유 규칙은 기존 RHI와 같다. 그래프는 명령을 제출하거나 프레임을 시작/종료하지 않는다.

callback 공유 데이터는 호출자가 동기화해야 한다. 그래프를 실행하는 동안 그래프·프레임·스왑체인을 변경하거나 같은 명령 목록을 다른 스레드에서 사용하면 안 된다. 첫 callback 전에 import와 pipeline 참조를 보유한다. callback 실패 또는 예외가 있으면 이미 시작한 작업을 모두 합류한 뒤 목록을 폐기한다. 예외는 호출 스레드로 전달된다. `nullptr` pool, 작업자 1개, 같은 pool 작업자 안에서의 중첩 호출은 현재 스레드에서 기록한다.

`GlobalBarrier()`는 자원을 선언하지 않은 패스에서도 등록 전후 모든 패스 사이에 실행 경계를 만든다. 초기화된 모든 **그래프 import**에 현재 상태를 유지하는 메모리 배리어를 기록한다. import하지 않은 외부 자원이나 여러 GPU 큐를 동기화하는 API가 아니다. 초기/최종 상태를 바꾸지 않으며 기존 백엔드의 같은 상태 배리어 구현을 사용한다.

실제로 병렬인 부분은 공개 RHI 명령과 callback의 CPU 기록이다. 현재 `Submit`은 장치 잠금 아래에서 네이티브 명령에 순서대로 재기록한다. 예제는 CPU 기록 시간과 CPU 제출 시간을 분리해 표시하며 GPU 병렬 실행이나 속도 향상을 주장하지 않는다. `--workers 0`은 하드웨어 작업자 수를 선택하고 `1`은 직렬 폴백을 검증한다.

`-DDY_BUILD_TESTS=ON`으로 `RenderGraphTests`를 빌드하면 작업자 실제 중첩, 무자원 전역 경계, 70개 독립 목록, 초기/최종 상태, GPU 업로드, callback 예외·기록 실패 복구, 소유 핸들 해제 후 참조 유지, 펜스 완료를 검증한다.
