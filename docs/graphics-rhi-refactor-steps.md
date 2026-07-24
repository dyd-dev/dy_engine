# Graphics · RHI · Backends 재정리 커밋 계획

## 단계 기준

- 한 번호는 독립적으로 설명하고 검토할 수 있는 최종 커밋 하나를 뜻한다.
- 현황 조사, 불일치 분류, 임시 안전장치, 같은 변경을 완성하기 위한 Backend별 작업은 별도 단계로 세지 않는다.
- 구현 중 fixup 커밋이 필요하더라도 그룹을 마칠 때 해당 단계의 커밋으로 합친다.
- 이미 끝난 A는 실제 7개 커밋을 그대로 기록한다. B 이후 공개 RHI 계약을 바꾸는 단계는 Null·Vulkan·D3D12·Metal의 번역까지 같은 최종 커밋에서 완결한다.
- 문서는 계속 수정되는 작업 계획이므로 각 그룹의 코드 커밋에는 포함하지 않는다.
- 빌드·실행·플랫폼 matrix 확인만 하는 작업은 번호로 세지 않고 관련 커밋의 검증으로 기록한다.
- 각 단계는 커밋 제목과 그 변경의 경계를 설명하는 한 줄로 쓴다. 완료 단계는 실제 제목, 미완료 단계는 예정 제목이다.

## 그룹과 적용 이점

| 그룹 | 범위 | 적용했을 때 얻는 이점 | 작업 브랜치 |
| --- | --- | --- | --- |
| A | Pass와 attachment 소유권 | Shadow를 특례로 만들지 않고 color-only·depth-only·color+depth pass를 같은 규칙으로 기록할 수 있다. 이후 RenderGraph가 shadow, depth pre-pass, post-process 등 일반 pass를 스케줄링할 기반이 된다. | `refactor-pass-attachment-ownership` |
| B | Device·frame·presentation 계약 | 요청값이 Backend 안에서 무시되거나 고정값으로 바뀌지 않으며, 초기화·frame 획득·표시 결과를 호출자가 예측할 수 있다. | `refactor-device-request-contract` |
| C | 명시적 RHI 계약 | 명시적 API의 pipeline·resource binding·barrier·rendering 정보를 cross-platform 공통 계약으로 보존하고 Backend는 native API 번역에 집중한다. | `refactor-explicit-rhi-contract` |
| D | Backend 구조와 GPU 수명 | 숨은 fallback과 거대한 Device 구현을 줄이고, 제출·동기화·파괴 시점을 각 Backend에서 추적 가능하게 만든다. | `refactor-backend-lifetimes` |
| E | Graphics 책임 경계 | Graphics는 mesh·material·stock rendering 정책을 소유하고 RHI는 모르게 한다. RenderGraph executor나 custom rendering을 추가해도 public API가 stock 구현 세부에 묶이지 않는다. | `refactor-graphics-boundary` |
| F | SDK·Model·통합 | `to_sdk`와 Model 합병 시 공개 헤더와 의존 방향을 지키고, 소스 트리 밖에서도 설치된 Graphics·RHI를 사용할 수 있게 한다. | `refactor-sdk-integration` |

## A. Pass와 attachment 소유권

1단계: `refactor(rhi)!: define depth-only pipelines`<br>
Shadow 전용 설정을 RHI에서 걷어내고 color·depth format 조합으로 depth-only pipeline을 표현한다.

2단계: `refactor(backends): support depth-only pipelines`<br>
Vulkan·D3D12·Metal이 공통 depth-only pipeline 계약을 각 native pipeline state로 번역한다.

3단계: `refactor(vulkan): preserve pass boundaries`<br>
Vulkan이 attachment 변경마다 draw 구간을 나누어 shadow pass와 main pass의 기록 순서를 보존한다.

4단계: `refactor(graphics): own stock shadow pass`<br>
Shadow depth texture·pipeline·pass 기록을 Vulkan 내부가 아니라 Graphics stock renderer가 관리한다.

5단계: `refactor(rhi)!: remove stock shadow path`<br>
Shadow 기능은 Graphics가 만든 일반 depth-only pass로 유지하고, 같은 기능을 중복하던 Vulkan 내부 구현과 RHI 전용 필드만 제거한다.

6단계: `refactor(graphics): expose shadow raster bias`<br>
고정된 shadow slope bias를 `RendererDesc`로 옮겨 다른 stock shadow 정책과 함께 설정하게 한다.

7단계: `refactor(backends): honor pass attachments`<br>
Backend의 암묵적 backbuffer·depth 대체를 없애고 호출자가 전달한 attachment만 사용하게 한다.

### A 그룹에서 해소한 Backend 불일치

| 항목 | A 적용 전 | A 적용 후 |
| --- | --- | --- |
| Depth-only pipeline | Vulkan은 shadow 전용 경로가 있었고 D3D12·Metal은 color target과 pixel shader를 사실상 가정했다. | 세 Backend 모두 일반 depth-only pipeline을 만들며 pixel shader를 요구하지 않는다. |
| Shadow 실행 소유권 | VulkanDevice가 stock shadow resource와 pass를 내부 생성했다. | Graphics가 stock shadow resource와 명령을 만들고 Backend는 일반 RHI 명령만 번역한다. |
| Pass 기록 순서 | Vulkan은 여러 `SetRenderTargets`와 draw 구간의 경계를 온전히 보존하지 않았다. | 호출 순서대로 attachment와 draw 구간을 보존한다. |
| Color attachment 없음 | Backend마다 실패하거나 shadow 특례로 처리했다. | 정상적인 depth-only pass로 처리한다. |
| 암묵적 depth target | Vulkan은 내부 shadow depth를, D3D12는 고정 D24 depth를 사용할 수 있었다. | 호출자가 전달한 depth attachment만 사용한다. |
| 잘못된 color fallback | D3D12는 backbuffer RTV를, Metal은 drawable을 대신 사용할 수 있었다. | 유효하지 않은 attachment는 pass를 무효화하며 다른 target으로 바꾸지 않는다. |
| Attachment 조합 | color-only·depth-only·color+depth 지원 범위가 Backend마다 달랐다. | 세 조합을 같은 RHI 계약으로 처리한다. |
| Shadow 전용 RHI 필드 | stock renderer 정책이 RHI pipeline과 Device 계약에 섞여 있었다. | 전용 필드를 제거하고 stock 설정은 Graphics에만 둔다. |
| Shadow raster bias | Graphics와 Backend의 고정값이 섞여 있었다. | Graphics 설정을 일반 raster state로 전달한다. |

## B. Device·frame·presentation 계약

### B 그룹에서 해소한 Device·frame·presentation 불일치

| 항목 | B 적용 전 | B 적용 후 |
| --- | --- | --- |
| Device 초기화 실패 | Backend 초기화 실패 뒤에도 사용할 수 없는 Device가 반환될 수 있다. | 부분 초기화된 Device를 폐기하고 `IDevice::Create()`가 `nullptr`을 반환한다. |
| `maxFramesInFlight` | `0`의 의미가 없고 일부 Backend는 요청과 무관하게 2개 frame slot을 사용한다. | `0`을 거부하고 미완료 제출의 상한이자 순환 재사용할 frame context 수로 적용하며 swapchain image 수와 분리한다. |
| Frame 획득 실패 | `BeginFrame()`이 실패를 표현하지 못해 timeout·out-of-date·drawable 부재가 조용히 누락될 수 있다. | `BeginFrame()`이 frame과 presentable image를 모두 획득했을 때만 `true`를 반환한다. |
| `swapchainFormat` | Backend마다 무시·강제·fallback 규칙과 실제 결과 보고 방식이 다르다. | `Unknown`은 native 선택, 구체 format은 정확히 지원하거나 실패하며 실제 선택값은 backbuffer가 보고한다. |

8단계: `fix(rhi): propagate device init failures`<br>
Backend 초기화가 실패하면 부분 초기화된 Device를 폐기하고 `IDevice::Create()`가 `nullptr`을 반환한다.

9단계: `refactor(rhi)!: define frames-in-flight limit`<br>
`maxFramesInFlight`를 미완료 제출의 상한이자 순환 재사용할 frame context 수로 정의하고 swapchain image index와 분리한다.

10단계: `refactor(rhi)!: report frame acquisition`<br>
`BeginFrame()`이 frame 획득 실패를 반환하게 하여 숨은 timeout과 조용한 frame 누락을 제거한다.

11단계: `refactor(rhi)!: define swapchain formats`<br>
`Unknown`은 native 선택, 구체 format은 정확히 지원하거나 실패하도록 하고 실제 결과는 backbuffer가 보고한다.

## C. 명시적 RHI 계약

진행 순서: `좌표계 고정 → shader binary → IShader 추가 → pipeline state(IA·raster/depth·color) → explicit resource binding(layout·sampler·resource set) → rendering scope → barrier`.

여기서 layout은 resource binding과 inline constant 범위를 선언하는 `PipelineLayoutDesc`만 뜻하며 vertex input·메모리·image layout은 각 단계의 별도 계약이다.

12단계: `refactor(rhi)!: require Vulkan 1.1 for Y-up`<br>
RHI 좌표계 규약을 하나로 고정하고 Vulkan 1.1의 core 음수 viewport로 native 좌표계에 번역하며, stock shadow 행렬과 shader의 clip-space→texture 좌표 변환도 같은 개념으로 통일한다.

13단계: `build(shaders): package native binaries`<br>
기존 예제별 `Shaders`에 누락된 Metal stock MSL asset을 공급하고 D3D12 HLSL은 DXIL로, Metal MSL은 Metal library로 build-time에 컴파일한다. 기존 runtime source 경로는 이 단계에서 유지하고 binary 소비와 객체 수명 전환은 다음 `IShader` 단계가 함께 맡는다.

14단계: `refactor(rhi)!: add shader interface`<br>
Stage·entry point·native binary view로 생성하는 독립 `IShader`를 추가하고 pipeline은 raw data/size 대신 shader 객체를 참조하게 한다. `IShader` public 계약과 모든 Backend 구현·Renderer 수명 전환은 중간 호환층 없이 한 커밋에서 함께 바꾼다.

15단계: `refactor(rhi)!: define input assembly state`<br>
Vertex binding·attribute·buffer slot·primitive topology와 vertex attribute용 scalar/vector `Format`을 호출자가 지정하게 하고 `BindVertexBuffer`·`BindIndexBuffer`를 모든 binding mode에서 native input assembly로 번역한다. 중복 `GeometryBinding`·`BindGeometry`와 stock vertex/index storage binding을 제거하며 빈 vertex layout에 vertex pulling이라는 특수 의미를 부여하지 않는다.

16단계: `refactor(rhi)!: expose fixed-function state`<br>
Rasterization과 depth-stencil state를 RHI pipeline에 명시하고 RHI clip-space winding을 native viewport 좌표로 번역하여, D3D12 main pass만 back-face culling하던 차이와 Vulkan의 `LessOrEqual`·D3D12/Metal의 `Less`로 갈리던 depth compare를 호출자 선택으로 통일한다.

17단계: `refactor(rhi)!: define color attachment state`<br>
Color attachment별 format·write mask·blend를 명시하고, D3D12/Vulkan의 고정 alpha blend와 Metal의 blend-off 차이 및 attachment 0만 처리하던 제한을 제거하여 MRT를 pipeline과 rendering 경로에서 지원한다.

18단계: `refactor(rhi)!: bind explicit resources`<br>
`PipelineLayoutDesc`로 set·binding·resource 종류·shader stage와 inline constant 범위를 선언하고 `ISampler`·resource set으로 실제 자원과 descriptor array를 묶는다. D3D12의 고정 root signature·static sampler·1024칸 global heap, Vulkan의 `DeviceDesc::shaderLayout` 기반 device-global set·draw 수 기반 pool·texture 소유 sampler, Metal의 고정 binding 배열·shader 내부 `constexpr sampler`를 호출자 선언에서 파생되는 pipeline별 layout과 필요한 크기의 resource set으로 통일한다. 세 계약을 같은 커밋에서 도입·소비하며 legacy 개별 bind와 descriptor slot API를 남기지 않는다.

19단계: `refactor(rhi)!: define rendering scopes`<br>
Attachment와 color·depth·stencil별 load·store·clear 동작을 `BeginRendering`에서 함께 지정하고 `EndRendering`으로 attachment 사용 범위를 닫아 기존 target·clear API를 대체한다. 이는 frame·thread·RenderGraph pass가 아니라 native render pass/encoder가 attachment를 사용하는 rendering scope다. D3D12의 scope 밖 clear 호출, Metal의 encoder 생성 전 pass descriptor 수정, Vulkan의 고정 clear/store render pass를 같은 호출자 계약으로 통일한다.

20단계: `refactor(rhi)!: expose resource barriers`<br>
`TextureBarrier`로 shader resource·render target·depth write·storage·present 전이를 rendering scope 사이에 호출자가 기록하게 한다. D3D12의 target bind·resource-set bind·command-list close 전이와 Vulkan render pass의 usage 기반 final layout 전이를 제거하고 Metal에서는 native hazard tracking에 맞춰 no-op으로 번역한다. `UpdateTexture`는 내부 copy 상태를 public 계약으로 노출하지 않고 호출자가 지정한 최종 state로 끝난다.

### C 그룹 수정 후의 계약

| 항목 | 수정 전 | 수정 후 |
| --- | --- | --- |
| Clip space | Vulkan Y 뒤집기가 행렬·shader·Backend에 분산되고 shadow 행렬만 별도 Y-down 규칙을 사용했다. | RHI는 RH, Y-up, Z `[0, 1]`로 고정하고 Vulkan 1.1 음수 viewport만 native 번역에 사용한다. GLSL shadow UV와 stock shadow 행렬도 같은 규약을 따른다. |
| Shader 공급 | Backend마다 source·binary 소비 방식과 runtime compile 의존성이 달랐다. | 예제 build가 기존 `Shaders` 폴더의 source를 DXIL·SPIR-V·metallib로 만들고 RHI는 native binary만 소비한다. runtime compiler 배포는 SDK 단계의 선택 기능으로 남긴다. |
| Shader 객체 | Pipeline desc가 raw pointer·size를 직접 들고 stage 및 수명이 불명확했다. | `IShader`가 stage·entry point·native binary 생성 계약과 수명을 소유하고 pipeline은 shader 객체를 참조한다. |
| Input assembly | storage-buffer vertex pulling과 `GeometryBinding`이 일반 IA처럼 섞여 있었다. | vertex binding·attribute·topology와 `BindVertexBuffer`·`BindIndexBuffer`를 명시하고 per-draw·batched·bindless 모두 native IA를 사용한다. |
| Fixed function | cull·front face·depth compare가 Backend 상수이거나 main pass 특례였다. | rasterization과 depth-stencil을 pipeline desc의 호출자 선택으로 만들고 winding만 native viewport에 맞춰 번역한다. |
| Color output | D3D12/Vulkan은 고정 alpha blend, Metal은 blend off였고 attachment 0 위주였다. | attachment별 format·write mask·blend와 MRT count를 pipeline 계약으로 통일한다. |
| Resource binding | D3D12 고정 root/global heap, Vulkan device-global layout/pool 및 texture-owned sampler, Metal 고정 배열·shader sampler가 공존했다. | pipeline별 `PipelineLayoutDesc`, 독립 `ISampler`, 필요한 크기의 resource set과 descriptor array로 통일한다. |
| Rendering 범위 | target 설정과 clear 호출이 분리되고 Backend마다 pass 시작 시점과 기본 load/store가 달랐다. | `BeginRendering`에서 attachment와 load·store·clear를 고정하고 `EndRendering`에서 native render pass/encoder 범위를 닫는다. |
| Resource state | target bind, descriptor bind, submit 직전 등에 Backend가 용도 기반 전이를 몰래 기록했다. | rendering scope 사이의 texture state 전이는 호출자가 barrier로 기록하며 upload만 지정한 최종 state로 끝난다. |

## D. Backend 구조와 GPU 수명

21단계: `refactor(graphics): own fallback textures`<br>
화면에 보이는 기본 texture 정책은 Graphics가 소유하고 Backend에는 native 유효성용 dummy만 남긴다.

22단계: `refactor(vulkan): split device responsibilities`<br>
거대한 `VulkanDevice.cpp`를 device·swapchain·resource·pipeline·submission 책임별 파일로 나누되 동작은 유지한다.

23단계: `fix(vulkan): enforce frame lifetime rules`<br>
Frame 획득·제출 실패 복구와 image·fence·descriptor 회수 시점을 GPU 완료 기준으로 맞춘다.

24단계: `fix(d3d12): enforce resource lifetimes`<br>
Upload와 resource 파괴를 fence 완료 뒤로 미루고 borrowed backbuffer의 소유권을 분리한다.

25단계: `fix(metal): enforce frame resource lifetimes`<br>
Drawable·command buffer·encoder·resource가 같은 frame 제출 수명 안에서 생성·종료·해제되게 한다.

## E. Graphics 책임 경계

26단계: `refactor(graphics)!: hide renderer internals`<br>
Renderer를 PImpl로 바꾸고 RenderPath 구현 타입을 public header에서 숨기되 Device와 frame lifecycle은 호출자가 유지한다.

27단계: `refactor(graphics): isolate submission policies`<br>
Geometry submission·batching·binding을 private policy로 분리하고 Single IA·Batched·Bindless는 preset으로만 둔다.

28단계: `refactor(graphics): internalize stock pass plan`<br>
Public placeholder `RenderPass`를 제거하고 shadow→main 실행 계획을 private으로 모아 RenderGraph 교체 경계를 남긴다.

29단계: `refactor(graphics): narrow GPU scene cache`<br>
`GpuScene`을 private texture residency cache로 축소하고 scene·texture 변경 시 cache 무효화를 명시한다.

30단계: `refactor(graphics): internalize shadow view`<br>
Stock shadow camera 계산을 private `StockShadowView`로 옮겨 일반 Graphics·RHI 계약에서 분리한다.

31단계: `refactor(graphics)!: hide stock shader ABI`<br>
Stock shader binding·payload·asset 해석은 private으로 숨기고 custom shader는 일반 RHI pipeline layout만 사용하게 한다.

## F. SDK·Model·통합

32단계: `refactor(sdk)!: define public header surface`<br>
공개 header allowlist와 Graphics·RHI umbrella를 만들고 외부 consumer로 header 독립성을 검증한다.

33단계: `refactor(model)!: isolate parser package`<br>
Model parser를 optional package로 분리하고 runtime Scene·Mesh·Material·Skeleton adapter만 Graphics에 연결한다.

34단계: `build: enforce target dependency boundaries`<br>
CMake target과 include visibility로 계층 역참조를 막고 third-party 구현 의존을 private으로 제한한다.

35단계: `build(shaders): package shader compiler`<br>
하나의 compiler core를 build-time CLI·CMake helper와 optional runtime library가 함께 사용하게 하고 SDK에서 source compile 기능을 선택적으로 설치할 수 있게 한다.

36단계: `build: export installable SDK targets`<br>
Static/shared symbol과 install config·runtime asset을 패키징하여 설치된 Graphics·RHI·optional Model·optional ShaderCompiler를 소비하게 한다.

## 작업 원칙

- 각 단계는 제목·설명에 명시된 계약과 그 계약을 모든 Backend에서 완결하는 직접 변경만 수행한다. 단계 밖의 정리·미래 확장·선제적 일반화는 구현하지 않고 불일치로 기록하며, 반드시 함께 바꿔야 한다면 구현 전에 단계 설명을 수정하거나 관련 단계와 병합한다.
- 새 public 계약이 다음 단계 전에는 실제로 사용할 수 없거나 다음 단계에서 곧 제거·변형할 adapter를 요구한다면 독립 단계로 두지 않고 최초 소비 단계와 합친다.
- Helper 함수와 `struct`·`class`의 추가를 극도로 경계한다. 코드 축약·이름 붙이기·일회성 분기·임시 단계 연결·미래 사용 가능성만으로 추가하지 않으며, 반복되는 정책·소유권·수명·불변식을 더 단순한 직접 코드로는 강제할 수 없을 때만 허용한다.
- `constexpr`·고정 capacity·이름 붙인 상수도 helper와 같은 기준으로 심사한다. 단일 사용값과 임의 한계는 그대로 두거나 설계를 고치고, ABI·shader layout·native 명세처럼 실제 compile-time 불변식에서 직접 파생될 때만 추가한다.
- 현재 예정된 Skinning Model, Single-Thread Render Graph, Tracy·RenderDoc profiler, library 문제 해결과 이후 Multi-Thread Render Graph, Mesh Shader, Ray Tracing이 도입될 것을 가정해 경계를 잡되 이 계획에서 그 기능 자체를 구현하지는 않는다.
- Ray Tracing처럼 일부 API에서만 가능한 실험 기능은 공통 RHI의 최소 공통분모를 왜곡하지 않고 Backend capability와 확장 인터페이스로 제공할 수 있게 한다.
- Backend에 숨은 외부 동작 고정값은 공개 계약이나 Graphics 정책으로 올리지만 allocator page 크기처럼 관찰 불가능한 tuning 값은 Backend private으로 남긴다.
- 새 public `Info`·`Desc` 구조체는 실제로 호출자가 선택하거나 결과를 관찰해야 할 때만 추가한다.
- 새 private helper·wrapper·내부 구조체도 반복되는 정책이나 불변식을 한곳에서 강제하거나 호출부의 의미를 분명하게 할 때만 추가하며, 일회성 분기·단순 치환을 옮기는 추상화는 만들지 않는다.
- Runtime RHI의 shader 생성은 Backend가 바로 소비할 binary만 받고 source compile을 숨겨 실행하지 않는다. Build-time과 runtime source compile은 같은 optional ShaderCompiler 계층을 명시적으로 호출한다.
- `IShader`와 `ISampler`처럼 독립 GPU 자원으로 재사용되고 수명·Backend 상태를 소유하는 객체만 public interface로 만들며, pipeline이나 resource set보다 오래 살아야 하는 참조 수명을 문서화한다.
- Graphics의 간결함은 RHI Device와 `BeginFrame()`·`EndFrame()`을 숨겨서 만들지 않는다. 일반 사용자는 Graphics helper를 쓰고 low-level 사용자는 같은 RHI lifecycle을 직접 제어할 수 있어야 한다.
- 실제 custom renderer 요구가 생기기 전에는 `RenderPacket` 같은 stock renderer 내부 자료형을 public API로 승격하지 않는다.
- 각 커밋은 관련 Null·Vulkan·D3D12 build와 example·direct-RHI 검증을 통과해야 하며 Metal은 native 환경에서 확인한다. 그룹 종료 시 optional Model과 installed SDK까지 통합 matrix를 확인하되 검증 자체를 별도 단계로 세지 않는다.
