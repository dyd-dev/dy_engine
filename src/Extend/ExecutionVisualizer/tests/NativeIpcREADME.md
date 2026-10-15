# Native IPC / rendering correctness driver

`ViewerNativeIpcTests`는 공통 `dyf::LiveClient`와 기존 `TargetProcess`를 사용합니다. 새로운 producer, 전송 schema 또는 UI 자동 입력을 만들지 않습니다. 대상은 `DY_ENABLE_LIVE_PROFILING=ON`으로 빌드된 실제 example14/15 D3D12 또는 Vulkan exe입니다.

```powershell
cmake --build build/live-viewer-optimization-d3d12 --config Release --target ViewerNativeIpcTests
ctest --test-dir build/live-viewer-optimization-d3d12 -C Release -R ViewerNativeDriver --output-on-failure
```

이 빌드 명령은 `dy_execution_visualizer`를 재링크하지 않습니다. CTest 항목은 인자·식별자 조인·history 중복/겹침·Failed 재시도 분류의 CPU self-check와 현재 Windows 프로세스의 OS 시간 조회를 실행합니다. native producer를 실행하지 않습니다.

실제 검증은 다음과 같이 별도로 실행합니다.

```powershell
& 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\tests\Release\ViewerNativeIpcTests.exe' `
  --example 14 `
  --target 'C:\GitHub\GitHub_Project\dy_engine\build\live-rhi-d3d12-on\example14\Release\AdvancedInstancingBatching.exe' `
  --cycles 2 --frames 900 --timeout 10 --exit-timeout 30 `
  --report 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\native-example14.txt'

& 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\tests\Release\ViewerNativeIpcTests.exe' `
  --example 15 `
  --target 'C:\GitHub\GitHub_Project\dy_engine\build\live-rhi-d3d12-on\example15\Release\AdvancedResourceReuse.exe' `
  --cycles 2 --frames 900 --timeout 10 --exit-timeout 30 `
  --report 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\native-example15.txt'
```

Vulkan은 `--target`을 해당 backend의 fresh exe로 바꿉니다. `--pid`는 실행 중인 별도 대상에 연결하며 `--target`과 함께 사용할 수 없습니다. 원본의 GUI 검증용 PID에는 연결하지 않습니다. `--arguments`와 `--working-directory`도 전달할 수 있습니다. 소유 실행에는 양수 `--frames`를 마지막에 추가해 유한 실행을 보장하고 자연 종료를 기다립니다. 강제 종료/창 입력은 하지 않습니다.

`--cycles 2`는 초기 실제 값과 반대인 값부터 네 번 교대로 요청하여 초기 값으로 돌아갑니다. 실패 시에도 초기 값을 복원하는 요청을 시도하고 결과를 기록합니다.

`--cycles 0`는 control을 전송하지 않는 관찰 모드입니다. 발견·연결·100ms snapshot 관찰·소유 프로세스 자연 종료 대기는 유지하며 초기 실제 mode가 바뀌지 않는지 확인합니다. 보고서는 `evidence=native_ipc_observation`, `control_correctness=not_exercised`로 구분합니다.

`--no-connect`는 소유 `--target`에만 적용하며 cycles를 0으로 고정합니다. 발견·연결·snapshot 조회·control 요청을 하지 않으므로 collector OFF 또는 ON disconnected의 유한 실행 baseline에 사용할 수 있습니다. `--frames`와 자연 종료 대기는 유지합니다. 같은 환경·backend·mode·frames의 OFF, ON disconnected, ON connected `--cycles 0`을 원본 채팅이 차례로 실행해 비교합니다.

```powershell
& 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\tests\Release\ViewerNativeIpcTests.exe' `
  --example 14 --target '<fresh OFF or ON exe>' --no-connect --frames 900 --exit-timeout 30 `
  --report 'C:\GitHub\GitHub_Project\dy_engine\build\live-viewer-optimization-d3d12\baseline-example14.txt'
```

각 소유 실행은 Windows `GetProcessTimes`로 target의 실제 생성→종료 CPU user/kernel 합과 벽시계 시간을 기록합니다. driver는 생성→보고서 작성 cutoff의 같은 값을 기록하며 최종 flush/종료는 포함하지 않습니다(`driver_measurement_complete=0`). target은 종료를 확인한 HANDLE에서 조회하며 미종료를 0으로 대체하지 않습니다. FILETIME은 100ns 단위이며 CPU 값은 전체 thread의 합입니다. [Microsoft GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)

history 감사는 한 snapshot의 source별 고유 `(revision, beginNs, endNs)` bucket만 합산합니다. 같은 revision의 겹치는 구간은 거부하고, 서로 다른 revision은 producer의 배타적 frame 분할에 따라 합산합니다. recent/history 간 또는 snapshot 간 중복 합산과 global frame ID 차분은 하지 않습니다. 이 수치는 현재 10초 history window이며 전체 실행 누계가 아닙니다.

소유 finite `--frames N`보다 history/recent frame 수가 크면 우선 경고합니다. 실제 적용 transition은 별도로 표시합니다. 무작업 Failed/Skipped 프레임은 온전한 실제 flow가 `BeginFrame / acquire` 실패/생략만 담을 때 재시도 후보가 됩니다. 이후 같은 source의 정상 Draw/Dispatch·Submit·Completed Present가 관찰되어야 확정하며, 다른 실패나 종료 시 실패를 일괄 재시도로 취급하지 않습니다. 예산 초과와 확정된 재시도 또는 steady 무작업 Completed가 함께 관찰되면 observation에서도 FAIL입니다. 100ms snapshot에 보이지 않은 재시도는 추정하지 않습니다.

각 실제 적용 revision에서 전환 후 0.6초가 지난 세 개의 온전한 frame을 검사합니다. CPU 기록과 실제 제출의 command list/generation/submission을 같은 원래 frame/revision의 완료된 `FrameCoverage` GPU span과 대조합니다. CPU loop의 Completed 표시만으로 GPU 완료를 인정하지 않습니다.

이 환경의 D3D12/Vulkan 검증에서는 실제 `Ready` timestamp 결과와 유효한 clock domain/queue interval까지 세 frame씩 필수입니다. fence 완료가 있어도 Unsupported/Unavailable이면 타이밍 성공으로 처리하지 않습니다. 보고서는 fence 원래-frame join과 Ready 원래-frame join을 따로 기록합니다.

- example14: Draw 256↔1, instances 256, 실제 Batch 이벤트 수 256↔1 및 Batch 요소 합 256, Draw 인자와 제출 집계 일치.
- example15: Draw 1, instances 256, Batch 1, 프레임마다 실제 upload 8192 B 유지. 현재 예제는 두 모드 모두 같은 데이터를 매 프레임 업로드합니다. steady reuse에서 실제 buffer 생성 누계가 유지되고 create에서는 증가하는지 검사합니다.
- Pending/Ready/Unsupported/Unavailable 관찰 수와 실제로 관찰된 Pending→Ready 전환을 별도로 기록합니다. GPU sample ID의 원래 frame/revision은 바뀔 수 없습니다. 미측정 시간을 0으로 치환하지 않습니다.
- 누락·불완전 상세는 보고서에 표시하고 해당 frame을 완료 증거로 사용하지 않습니다. snapshot sequence 간격을 실행 frame 수로 해석하지 않습니다.

이 driver 보고서의 증거 범위는 native IPC·렌더링 동작입니다. GUI 입력·표시 지연·GUI 자체 비용은 root가 별도로 수행한 [최종 GUI 실측](../README.md#최종-gui-실측-2026-10-15)에 기록했습니다. D3D12 example14의 실제 UI 전환 20회는 p95 185.869 ms로 250 ms 목표를 충족했고, 최대 316.368 ms의 초과 표본 1회가 있었습니다. Vulkan example15의 4회는 기능·보조 지연 검증으로 구분합니다. 두 GUI 실행은 수정된 GLFW와 같은 Release GUI 해시에서 정상 종료했습니다. 이 결과를 driver 자체의 GUI 입력 증거나 scan-out 측정으로 확대하지 않습니다.
