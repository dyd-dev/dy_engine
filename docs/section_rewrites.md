# dy_engine 보고서 — 섹션 재작성 통합본 (단일 원고)

---

# Ⅰ.1 개발 배경 및 필요성

## 1) 레거시 객체 지향(OOP) 아키텍처의 한계
대다수 상용 게임 엔진은 과거 프로젝트와의 호환을 위해 객체 지향(OOP) 기반 씬 그래프와 두꺼운 추상화 계층을 유지합니다. 다중 상속과 가상 함수 중심의 구조는 객체를 메모리에 흩어 놓아, 런타임에 포인터 추적(pointer chasing)과 가상 호출을 반복하게 만듭니다. 이러한 접근 패턴은 CPU 캐시 미스를 늘려, 코어 수가 늘어도 처리량이 비례해 오르지 못하는 원인이 됩니다. (캐시 미스가 성능에 미치는 메커니즘과 측정은 Ⅲ.1에서 다룹니다.)

## 2) 현대 그래픽스 API의 하드웨어 제어 철학
DirectX 12와 Vulkan은 드라이버가 암묵적으로 처리하던 상태 전이를 걷어내고, 메모리와 파이프라인의 통제권을 프로그래머에게 넘깁니다. 그러나 다수의 오픈소스 프레임워크는 이 명시적 API 위에 구형 OpenGL 스타일의 객체 지향 래퍼를 덮어, API가 의도한 캐시 친화적 이점을 다시 가립니다. 따라서 레거시 호환과 범용 래퍼를 걷어내고 하드웨어 캐시 구조에 맞춘 데이터 지향 프레임워크가 필요하며, 본 프로젝트는 이를 목표로 합니다.

---

# Ⅰ.2.1 시스템 아키텍처 구성도

본 프레임워크는 다음 4개 계층과 횡단 관심사로 구성되며, 최종 목표를 향해 단계적으로 확장합니다. (구성도는 정적 구조의 지도이며, 모듈별 상세 설계는 Ⅳ.2에서 다룹니다.)

```
┌──────────────────────────────────────────────────────────────┐
│  Application / Examples                                        │
└───────────────────────────────┬──────────────────────────────┘
                                 ▼
┌─ Layer 4 · Render Front-End ──────────────────────────────────┐
│  Renderer · Scene/Material · Flattened Render Queue(Sort Key)  │
│  · Bindless 바인딩                                             │
├─ Layer 3 · Render Graph (프레임 그래프) ──────────────────────┤
│  Pass 선언 · 리소스 수명/배리어 자동화 · Transient 풀 · MT 기록 │
├─ Layer 2 · RHI ───────────────────────────────────────────────┤
│  IDevice · ICommandList(Thread-Local) · Resources             │
│  Capabilities: Graphics / Compute / Mesh / Ray Tracing        │
│  Backends: Vulkan · D3D12 · Metal · Null                      │
├─ Layer 1 · Foundation ────────────────────────────────────────┤
│  Math(SIMD) · Platform · Core · Job/Task System(멀티스레딩)    │
└───────────────────────────────────────────────────────────────┘
횡단 관심사: Multi-Threading(Job→스레드별 기록) · Frame Indexing(더블/트리플 버퍼링)
```

단계별 개발 일정과 진척률은 Ⅴ.1 기능별 진척 사항에서 다룹니다.

---

# Ⅰ.2.2 시스템 서비스 시나리오

프레임워크 사용자(렌더러를 개발하는 프로그래머) 관점에서 본 프레임워크를 사용하는 전형적 상황과 그 과정에서 얻는 강점은 다음과 같습니다. 각 항목은 [상황] → [사용 방식] → [강점] 순으로 기술합니다.

- **멀티 플랫폼 출시** — 사용자는 백엔드별 코드를 따로 작성하지 않습니다. `IDevice::Create` 한 번으로 실행 플랫폼에 맞는 백엔드(Vulkan·D3D12·Metal)가 선택되며, 이후 리소스 생성과 드로우 코드는 백엔드와 무관하게 동일합니다. → **강점: 단일 코드베이스로 다중 플랫폼 이식.**
- **성능 튜닝과 직접 제어** — 기본값은 최적으로 제공하되, 필요에 따라 세 깊이 중 선택합니다: ① `RendererDesc`로 바인딩 경로·포맷·조명·그림자·Bindless 등 **모든 설정**을 조정, ② 상위 렌더러의 저수준 API로 엔진 편의를 우회, ③ `IDevice`·`ICommandList`(RHI)를 직접 사용해 렌더러를 거치지 않고 커맨드를 기록. → **강점: 디폴트로 즉시 시작하고 필요하면 어느 깊이로든 내려가는 점진적 제어(progressive disclosure).**
- **대규모 오브젝트 렌더링** — 전역 디스크립터 힙과 인라인 상수(Push Constants)를 통해 슬롯 바인딩 오버헤드 없이 다수의 오브젝트를 그립니다. → **강점: 명시적 자원 모델의 직접 노출.**
- **(예정) 멀티코어 활용** — 워커 스레드별 독립 커맨드 리스트로 드로우 콜을 병렬 기록합니다(설계·진행 중). → **강점(로드맵): 멀티코어 처리량.**

이처럼 본 프레임워크는 점진적 제어, 곧 추상화를 천장이 아니라 선택지로 두는 설계를 강점으로 삼습니다. 사용자는 디폴트로 시작하고 필요한 만큼만 config·상위 우회·RHI 직접으로 내려갑니다. 이 강점이 기존 라이브러리 대비 어떻게 차별화되는지는 다음 장(Ⅱ. 관련 작품 분석)에서 비교합니다.

---

# Ⅱ. 관련 작품 분석

## 1. 관련 작품 설명

### 1) bgfx
bgfx는 광범위한 플랫폼과 그래픽스 API를 단일 인터페이스로 묶어 제공하는, 사실상 업계 표준급으로 널리 쓰이는 렌더링 라이브러리입니다. 다만 DirectX 9·OpenGL·WebGL 등 레거시 상태 기계(state machine) 계열 API와의 하위 호환을 유지하기 위해 내부에 '최소 공통 분모(lowest common denominator)' 기반의 추상화 계층을 둡니다. 이 때문에 Vulkan·DirectX 12가 제공하는 명시적 메모리 제어(Bindless Heap)나 락프리(Lock-Free) 커맨드 기록 같은 최신 기능을 응용 단계에서 직접 통제하기 어렵고, API 간 차이를 흡수하기 위한 내부 상태 변환(translation) 비용이 더해집니다. 또한 이 추상화 계층 아래의 명시적 제어로 직접 내려갈 경로가 제공되지 않습니다.

### 2) Methane Kit
Methane Kit은 DirectX 12·Vulkan·Metal 등 최신 명시적 API만을 대상으로 설계된 현대적 프레임워크입니다. 다만 아키텍처가 객체 지향(OOP) 패러다임에 기반하여, 렌더링 리소스와 커맨드 컨텍스트가 깊은 상속 구조와 스마트 포인터·가상 함수 테이블(vtable)로 래핑됩니다. 객체 단위 추상화의 편의는 높지만, 포인터 추적(pointer chasing)과 가상 호출이 늘어 멀티코어 환경에서 캐시 효율에 부담이 될 수 있습니다. 추상화 깊이가 OOP 객체 계층으로 고정되어, 그 계층을 우회한 저수준 제어가 어렵습니다.

## 2. 차별성 및 우수성
앞 절에서 제시한 점진적 제어(디폴트→config→상위 우회→RHI 직접), 단일 코드베이스 이식성, 명시적 자원 모델의 직접 노출은 위 두 라이브러리가 공통으로 갖는 한계, 곧 고정된 추상화 깊이에 대응합니다. 주요 차이를 요약하면 다음과 같습니다.

| 항목 | bgfx | Methane Kit | 본 프레임워크 |
|---|---|---|---|
| 대상 API | 레거시~최신 광범위 | 최신 명시적(DX12·VK·Metal) | 최신 명시적 + Null(검증) |
| 추상화 방식 | LCD 추상화 + 상태 변환 | 깊은 OOP 상속 / vtable | 얇은 데이터 컨테이너 |
| Bindless·디스크립터 힙 | 간접·제한적 | 부분 지원 | 전역 힙 1급 노출 |
| 바인딩 전략 | 고정 | 고정 | PerDraw·Batched·Bindless 교체 |
| 제어 깊이(우회 경로) | 추상화 고정·우회 난해 | OOP 깊이 고정 | 디폴트→config→상위 우회→RHI 직접 |
| 설계 우선순위 | 호환성·이식성 | 현대성 + OOP 편의 | 처리량·하드웨어 제어 |

표에서 보듯 bgfx의 '최소 공통 분모' 추상화와 상태 변환 비용은 본 프레임워크에서 얇은 데이터 컨테이너 설계로 대체되고, Methane Kit의 깊은 OOP 상속·가상 호출 부담은 데이터 지향 구조로 대체됩니다. 전역 디스크립터 힙 기반 Bindless와 바인딩 경로 교체는 두 라이브러리에서 고정되어 응용 단계에서 선택·측정할 수 없습니다. 두 라이브러리는 추상화 깊이가 고정되어 그 아래로 내려갈 수 없는 반면, 본 프레임워크는 디폴트·config·상위 우회·RHI 직접의 네 깊이를 사용자가 선택하는 점진적 제어를 제공합니다.

요약하면 본 프레임워크의 우수성은 '모든 API를 포괄하는 범용성'이 아니라 '최신 명시적 API의 이점을 가리지 않는 얇고 데이터 지향적인 설계'에 있으며, 이는 호환성을 위해 추상화 비용을 감수하는 bgfx, 현대성을 택하되 OOP 비용을 안는 Methane Kit과 구분됩니다.

---

# Ⅲ.1 현대 멀티코어 하드웨어 친화적 아키텍처에 관한 연구

> Ⅲ.2~4(DX12·Metal·Vulkan 백엔드 구현)는 각 담당 팀원의 영역이라 본 원고에 포함하지 않습니다.

현대 CPU에서 렌더링·시뮬레이션의 병목은 연산량(ALU)보다 주 메모리 접근 지연(memory-bound)에 있습니다. 코어는 한 사이클에 여러 명령을 처리하지만 주 메모리 접근은 수백 사이클이 걸려, 그동안 코어가 데이터를 기다리며 놀게 됩니다. 따라서 성능은 캐시 적중률에 크게 좌우되며, 본 연구는 데이터를 캐시 친화적으로 배치하는 설계 원리를 검토하고 그 효과를 측정합니다.

본 절의 논지는 세 갈래의 선행 연구가 수렴하여 뒷받침합니다.

| 선행 연구 | 관점 | 핵심 기여 |
|---|---|---|
| Drepper (2007) | 하드웨어 메커니즘 | 캐시 계층과 미스 비용의 원리 — '왜 메모리가 병목인가' |
| Albrecht (2009) | 게임 도메인 실측 | OOP 씬 그래프 순회의 L2 미스와 프레임 시간(19.2→3.3ms) 측정 |
| Nikolov (2018, CppCon) | 현대 패러다임 | OOP의 숨은 비용과 데이터 지향 설계(DOD)를 일반화한 업계 관점 |

세 연구는 하드웨어 원리(Drepper) → 게임 도메인 실측(Albrecht) → 현대 패러다임(Nikolov)으로 이어지며, 본 프로젝트의 데이터 지향 설계 결정을 원리·실측·업계 동향의 세 측면에서 뒷받침합니다.

## 1) 데이터 지향 설계(DOD)에 입각한 SoA(Structure of Arrays) 구조
객체 지향에서 흔한 AoS(Array of Structures)는 한 객체의 모든 필드를 묶어 배열로 둡니다. 그러나 특정 필드만 순회하는 작업(예: 위치 갱신)에서는 캐시 라인에 함께 끌려온 나머지 필드가 사용되지 않아 라인의 유효 바이트율이 떨어집니다. 반대로 SoA는 같은 필드를 연속 배열로 분리하므로, 해당 작업이 읽는 데이터가 캐시 라인을 가득 채웁니다. 이러한 포인터 추적과 연속 배열의 대비는 데이터 지향 설계(DOD)의 출발점입니다(Acton, 2014).

```cpp
// AoS: pos만 갱신해도 vel·mass가 같은 캐시 라인을 차지(낭비)
struct Entity { float3 pos; float3 vel; float mass; /* ... */ };
std::vector<Entity> e;
for (auto& x : e) x.pos += x.vel * dt;

// SoA: pos·vel 배열만 선형 접근 → 라인 유효 바이트율↑, SIMD 적재 용이
std::vector<float3> pos, vel;
for (size_t i = 0; i < n; ++i) pos[i] += vel[i] * dt;
```

## 2) CPU 캐시 아키텍처 및 물리적 메모리 레이아웃
메모리는 단일 속도가 아니라 계층(L1→L2→L3→주 메모리)으로 구성되며, 하위로 갈수록 용량은 크지만 지연이 급격히 늘어납니다. 한 번의 미스로 주 메모리까지 내려가면 수백 사이클, 곧 수백 개 명령을 실행할 시간을 대기로 소모합니다(Drepper, 2007). 데이터는 바이트 단위가 아니라 캐시 라인 단위(x86 64바이트, Apple Silicon 128바이트)로 이동하므로, 라인 안의 데이터를 얼마나 함께 쓰느냐(공간 지역성)가 성능을 가릅니다.

| 계층 | 대략적 접근 지연 | 비고 |
|---|---|---|
| L1 | 약 4 사이클 | |
| L2 | 약 12 사이클 | |
| L3 | 약 40 사이클 | |
| 주 메모리 | 약 100–300 사이클 | 1회 미스 = 수백 명령 분량 대기 |

(대략치이며 CPU에 따라 다릅니다. 출처: Drepper, 2007.) 객체 지향 씬 그래프처럼 포인터로 흩어진 객체를 따라가는 접근(pointer chasing)과 가상 함수 테이블 간접 호출은 매 접근이 미스가 되기 쉽습니다. Albrecht(2009)는 OOP 씬 그래프 순회에서 객체당 다수의 L2 미스를 측정하고, 데이터 지향 재배치로 프레임 시간을 19.2ms에서 3.3ms로 단축한 사례를 보고합니다.

## 3) SIMD 인트린직(Intrinsics)을 활용한 데이터 병렬 처리
SoA는 캐시 지역성뿐 아니라 SIMD 활용에도 유리합니다. 같은 타입의 데이터가 연속으로 놓이면 SIMD 레지스터에 그대로 적재(load)하여 한 명령으로 여러 요소를 처리할 수 있는 반면, AoS는 흩어진 필드를 모으는 gather·shuffle 비용이 듭니다. SIMD 폭은 128비트(SSE, float 4개)·256비트(AVX, 8개)·512비트(AVX-512, 16개)로 넓어지며, 폭이 커질수록 정렬 요구(각각 16·32·64바이트)도 커집니다. 본 엔진은 현재 SSE(128비트, `__m128`)를 사용하며, 그 효과는 4)에서 측정합니다. AVX·AVX-512로의 폭 확장은 향후 과제이고, 정렬·인트린직의 플랫폼 차이는 SIMD on/off 빌드 구성으로 검증합니다.

## 4) AoS·SoA·SIMD 순회 성능 측정
위 원리의 효과를 본 프로젝트 개발 환경에서 직접 측정했습니다. 표 1은 메모리 레이아웃(AoS/SoA)의 효과를, 표 2는 SIMD의 효과를 봅니다. (표 1은 g++ `-O3 -march=native`로 배열 크기를 캐시 용량 안팎으로 키우며 순회 시간을 비교했습니다 — 하드웨어 카운터 대신 이 방식으로 지역성을 관찰. 표 2는 엔진 실제 빌드와 같은 MSVC `/O2`로 07_RenderPath에 내장된 행렬곱 벤치를 측정했습니다.)

**표 1. 메모리 레이아웃 — `pos += vel·dt` (float3, scalar, 요소당 ns)**

| 배열 크기(AoS 기준) | AoS | SoA | SoA 우위 |
|---|---|---|---|
| 0.5 MB (캐시 내부) | 1.04 ns | 1.07 ns | 0.97× |
| 4 MB (L2~L3) | 1.09 ns | 1.09 ns | 1.00× |
| 32 MB (L3 초과) | 5.44 ns | 1.20 ns | **4.52×** |
| 256 MB (주 메모리) | 5.47 ns | 1.39 ns | **3.95×** |

데이터가 캐시에 들어가면 레이아웃 차이가 사실상 없으나(≈1×), L3를 초과하는 순간 AoS가 4~5배 느려집니다. AoS가 갱신에 쓰지 않는 필드(mass·패딩)까지 캐시 라인째 끌어와 메모리 대역폭을 낭비하기 때문이며, 따라서 대규모 씬일수록 SoA의 이점이 커집니다.

**표 2. SIMD — `float4x4` 행렬곱 200만 회 (07_RenderPath 내장 벤치, MSVC `/O2`)**

| 구성 | 시간(ms) | 상대 |
|---|---|---|
| SIMD OFF (scalar) | 31.87 | 1.00× |
| SIMD ON (SSE) | 14.68 | **2.17×** |

행렬곱처럼 연산 강도가 높고 데이터가 캐시에 상주하는 커널에서는 명시적 SSE가 스칼라 대비 약 2.2배 빠릅니다. 이는 `build_vk`(SIMD)와 `build_vk_nosimd`(스칼라) 빌드 사이에서 관측된 프레임률 차이와 일치합니다.

단, SIMD 이득은 두 가지에 좌우됩니다. (1) **컴파일러** — 본 프로젝트 빌드인 MSVC `/O2`는 스칼라 행렬곱을 자동 벡터화하지 않아 명시적 SSE가 약 2.2배 이득을 주지만, GCC `-O3`처럼 스칼라를 자동 벡터화하는 컴파일러에서는 격차가 거의 사라집니다. (2) **커널 특성** — 대용량 스트리밍처럼 메모리 대역폭에 묶인 연산에서는 SIMD 폭을 넓혀도 이득이 작습니다(병목이 연산이 아니라 메모리).

종합하면, 큰 성능 이득은 두 축에서 나옵니다. 대규모 데이터에서는 레이아웃(SoA)이, 연산 집약 커널에서는 SIMD가 결정적입니다. 두 결과 모두 "병목은 연산보다 메모리 접근"이라는 본 절의 논지와 일관됩니다.

---

# Ⅳ.2 기능 설계

본 절은 Ⅲ.1의 연구(캐시·SoA·SIMD)를 실제 엔진 설계로 구체화합니다. 설계는 네 방향을 향합니다: ① 데이터 지향(캐시 적중률), ② 얇은 명시적 추상화와 점진적 제어(직접 통제), ③ 멀티코어 처리량, ④ 확장성. 아래 여섯 영역이 데이터 레이아웃 → RHI → 실행 모델 → 렌더링 → 확장 → 설계 원칙 순으로 이 방향에 대응합니다.

## 1) 데이터 지향 레이아웃 (SoA)
Ⅲ.1에서 분석한 캐시·SoA 원리에 근거하여, 객체 지향의 `GameObject` 배열을 폐기하고 데이터를 용도별로 분리해 연속된 메모리에 배치합니다. Transform(Position, Rotation)·Physics(Velocity, Mass)·Material(Index, Parameters)을 각각 별도의 플랫 배열로 관리하므로, 예를 들어 물리 갱신 시 CPU는 필요한 Velocity·Mass 배열만 선형으로 읽습니다. 그 결과 캐시 라인 단위로 연속 접근하여 적중률을 높이며, 16바이트로 정렬된 데이터는 SIMD(SSE) 레지스터에 적재되어 한 명령으로 4개 요소를 처리합니다. (캐시 라인 크기는 x86 64바이트, Apple Silicon 128바이트로 플랫폼마다 다릅니다.)

## 2) RHI (Render Hardware Interface)
명시적 API(Vulkan·D3D12·Metal)를 얇고 투명하게 래핑하는 계층입니다. 그래픽스뿐 아니라 향후 도입할 Compute·Mesh·Ray Tracing 커맨드까지 동일한 인터페이스에서 capability 단위로 노출하도록 설계합니다.

| 핵심 메커니즘 | 세부 기술 | 설계 목적 |
|---|---|---|
| Thread-Local RHI | 스레드별 독립 `ICommandList`·할당자 | 병렬 커맨드 기록 시 전역 경합 제거 |
| Resource Factory | `IBuffer`·`ITexture` 핸들 추상화 | 백엔드 구현체 은닉·모듈 격리 |
| Capability 기반 확장 | Graphics/Compute/Mesh/RT 노출 | 고급 파이프라인 도입의 단일 진입점 |

- **IDevice (Central Hub).** 스왑체인 관리, 리소스 생성, 전역 동기화의 정점입니다. 모든 리소스는 이 팩토리로만 생성되며 백엔드 구현은 외부에 노출되지 않습니다.
- **ICommandList (Thread-Local).** 워커 스레드는 `AcquireCommandList()`로 전용 기록기를 확보합니다. 가상 함수 오버헤드를 줄이기 위해 인라인화된 백엔드 호출을 지향하며, `DispatchCompute()`·`DrawMeshTasks()`·`DispatchRays()`는 로드맵에 따라 단계적으로 추가합니다.
- **Resource Management.** `BufferUsage`·`TextureUsage` 비트마스크로 용도를 정의해 GPU 힙(Upload/Default) 배치를 최적화합니다.
- **점진적 제어의 최하단 지점.** RHI는 사용자가 직접 내려올 수 있는 가장 낮은 통제 계층입니다. 상위 렌더러의 기본값으로 시작해, 필요에 따라 `RendererDesc` config → 상위 렌더러의 저수준 API → `IDevice`·`ICommandList` 직접 사용 순으로 원하는 깊이까지 제어를 가져갈 수 있습니다(Ⅱ장에서 제시한 점진적 제어 강점의 설계 근거).

## 3) Multi-Threading 및 Task Scheduling
CPU의 모든 코어를 활용하기 위한 실행 모델로, 연산을 태스크 단위로 분산합니다. 로직·렌더 스레드 간 데이터 경합을 막는 상태 다중화(State Separation)도 이 영역에 속합니다.

| 핵심 메커니즘 | 세부 기술 | 설계 목적 |
|---|---|---|
| Task System | 의존성 그래프 기반 Job 스케줄링 | 멀티코어 처리량 활용 |
| 병렬 커맨드 기록 | Thread-local `ICommandList` | 수만 드로우 콜 동시 기록 |
| State Separation | Frame Indexing·버퍼 다중화 | 로직–렌더 스레드 동기화 락 제거 |
| 제출 순서 보장 | 위상 정렬 + Sort Key 병합 | 데이터 일관성·결정적 출력 |

- **Task System (Job System).** 연산을 태스크로 분산하고 Culling→Sorting→Command Recording→Submit의 선행 관계를 의존성 그래프로 정의합니다.
- **상태 분리 (State Separation).** `IDevice::GetCurrentFrameIndex()`로 프레임 번호(0,1,2)를 얻어 갱신이 빈번한 버퍼를 프레임 수만큼 생성합니다. 시뮬레이션 스레드가 인덱스 N에 쓰는 동안 GPU는 N−1을 읽도록 격리되어, 별도 Mutex 없이 두 스레드가 동시에 동작합니다.
- **병렬 커맨드 기록 및 순서 보장.** 스레드별 독립 메모리로 여러 코어가 동시에 기록하고, 최종 제출 시 위상 정렬로 선행 태스크 완료를 보장합니다. Sort Key를 기준으로 커맨드 리스트를 정렬·병합해 단일 배치로 제출하므로, 멀티스레드 기록에도 출력은 결정적입니다.

## 4) 렌더링 파이프라인 및 Render Graph
상위 렌더러와 하위 RHI를 잇는 데이터 통로입니다. 렌더 패킷 생성·정렬과 Bindless 바인딩으로 드로우를 처리하고, 패스 간 의존성·리소스 수명·배리어는 Render Graph가 자동 산출합니다.

| 핵심 메커니즘 | 세부 기술 | 설계 목적 |
|---|---|---|
| Flattened Render Queue | 64-bit Sort Key 패킷 | 상태 변경 최소화·드로우 콜 최적화 |
| Bindless 전략 | Global Descriptor Heap + Push Constants | 슬롯 바인딩 제거·O(1) 리소스 접근 |
| Render Graph | 패스의 읽기/쓰기 리소스 선언 | 실행 순서·배리어 자동화, Transient aliasing |

- **Flattened Render Queue.** 정렬 가능한 평탄 패킷을 생성하며 Sort Key는 [Depth(24)|PSO ID(24)|Instance ID(16)]입니다. 불투명 객체는 Front-to-Back로 정렬해 Overdraw를 줄이고 동일 PSO 패킷을 묶어 상태 변경을 최소화합니다.
- **Bindless 전략.** `BindGlobalDescriptors()`로 단일 힙을 한 번 등록하고, `AllocateDescriptorSlot()`이 준 인덱스를 셰이더에 직접 전달합니다. 소량 데이터는 `SetInlineConstants()`(Push Constants)로 버퍼 없이 주입합니다.
- **Render Graph.** 각 패스가 사용할 리소스를 읽기/쓰기로 선언하면 그래프가 실행 순서와 `ResourceBarrier`를 자동 산출합니다. G-buffer 등 일시적 리소스는 Transient 풀에서 aliasing 재사용하며, 3절의 멀티스레드 패스 기록과 결합됩니다.

## 5) GPU 연산 확장 (Compute·Mesh Shader·Ray Tracing) — 확장 계획
명시적 API의 최신 파이프라인을 단계적으로 도입해 CPU 의존을 줄이고 처리량을 높입니다. (현재 미구현, 로드맵 Phase 4~6.)

| 핵심 메커니즘 | 세부 기술 | 설계 목적 |
|---|---|---|
| Compute Shader | DispatchCompute, GPU 컬링·포스트프로세싱 | CPU 병목 완화·대역폭 활용 |
| Mesh Shader | Task→Mesh 파이프라인, meshlet | GPU-driven geometry, 드로우 콜 급감 |
| Ray Tracing (도전) | BLAS/TLAS, ray query·RT 파이프라인 | 하이브리드 그림자·반사·AO |

- **Compute Shader.** Compute PSO와 `DispatchCompute()`를 추가해 프러스텀·오클루전 컬링, tiled/clustered 라이트 컬링, bloom·tone mapping을 GPU에서 수행합니다.
- **Mesh Shader.** 고정 IA·VS 단계를 Task→Mesh 셰이더로 대체하고 지오메트리를 meshlet 단위로 분할해 GPU에서 직접 컬링·증폭하므로 드로우 콜이 크게 줄어듭니다.
- **Ray Tracing (도전 과제).** BLAS/TLAS 가속 구조를 빌드하고 RT 파이프라인 또는 inline ray tracing으로 그림자·반사·AO를 하이브리드로 처리하며, DXR과 `VK_KHR_ray_tracing`에 매핑합니다.

## 6) 설계 원칙 — 모듈 경계 (Module Boundaries)
- **Header Isolation.** RHI 모듈은 플랫폼/백엔드 헤더(`Windows.h`, `vulkan.h`)를 노출하지 않으며 구현은 Internal 클래스·Pimpl로 은닉합니다.
- **Data-Only Communication.** 모듈 간 통신은 무거운 객체 대신 POD 구조체나 인덱스(ID)로 하며, 직렬화·네트워크 전송에도 유리합니다.

## * 병목(Bottleneck) 시나리오
병목은 단일 기능이 아니라 CPU 제출·GPU 실행·메모리 접근 전 구간에서 발생하며, 각 설계 요소가 이를 분담해 해소합니다.

| 병목 지점 | 원인 | 대응 기능 |
|---|---|---|
| CPU 드로우 콜 제출 | 단일 스레드 기록 | Multi-Threading(병렬 기록) |
| GPU 파이프라인 스톨 | 과다·중복 배리어 | Render Graph 자동 배리어 + PSO 베이킹 |
| 지오메트리 처리량 | 고정 IA·과다 드로우 콜 | Mesh Shader(meshlet) |
| 씬 컬링 CPU 비용 | CPU frustum/occlusion | Compute Shader GPU 컬링 |
| 메모리 대역폭·단편화 | 산재된 데이터·잦은 동적 할당 | SoA 구조와 연속 배열 배치 |

요약하면 본 엔진은 단일 기능 최적화가 아니라 CPU 제출–GPU 실행–메모리 접근 전 구간의 병목을 각 설계 요소가 분담해 해소하는 구조를 지향합니다.

---

# Ⅴ.1 기능별 진척 사항 (p.23 표)

> 템플릿 잔재(DB 설계/UI/UX)를 실제 엔진 기능으로 교체하고, 구성도에 있던 개발 로드맵(Phase)을 이 표로 통합했습니다. 진척률(%)은 팀이 마일스톤 기준으로 확정.

| 단계 | 기능 | 진척 내용 | 진척률(%) |
|---|---|---|---|
| Phase 1 | Foundation (Math/Platform/Core) | SIMD 수학, 윈도우·입력·타이머 | 90 |
| Phase 1 | RHI 추상화 + 백엔드 | IDevice/ICommandList, Vulkan·D3D12·Metal·Null | 80 |
| Phase 1 | 렌더러 & 바인딩 전략 | Forward+Shadow, PBR, PerDraw·Batched·Bindless | 70 |
| Phase 2 | Multi-Threading (Task System) | Thread-local 커맨드 기록, Job 의존성 그래프 | 30 |
| Phase 3 | Render Graph | 패스/배리어/transient 자동화 | 15 |
| Phase 4 | Compute Shader | DispatchCompute, GPU 컬링·포스트프로세싱 | 10 |
| Phase 5 | Mesh Shader | meshlet, GPU-driven geometry | 5 |
| Phase 6 | Ray Tracing (도전) | BLAS/TLAS, 하이브리드 RT | 0 |

---

# Ⅴ.2 개인별 자기평가 — 2.1 한재승(20242215)

## 수행 내용 (요약)
- RHI 인터페이스 설계·합의 — 백엔드(Vulkan·D3D12·Metal) 담당 팀원과 합의하여 각 API의 공통 계약을 RHI(`IDevice`·`ICommandList` 등)로 추상화. (최신 명시적 API의 직접 구현은 백엔드 담당이, 본인은 인터페이스 설계를 담당)
- SoA를 고려한 데이터 레이아웃과 SIMD(SSE) 수학 모듈(Math) 설계.
- 크로스플랫폼 빌드 구성(CMake; DX/VK·SIMD on/off·Metal)과 플랫폼 계층(Window·Input·Time).
- 렌더러 골격과 바인딩 전략 계층(`IRenderPath`) 설계.
- 프로젝트 방향 설정·아키텍처 의사결정 주도, 기능 분배(Light·Shadow·Model·Mesh를 팀원에 위임).

## 과제수행 시 문제점 및 해결방안
- [문제] 최소 RHI로 시작했으나 백엔드별로 필요한 기능이 계속 튀어나와(기능 범람) 계약을 반복 갱신해야 했고, 모든 메서드를 순수 가상(pure virtual)으로 강제하니 인터페이스를 바꿀 때마다 전 백엔드를 동시에 수정해야 해 변경 비용이 커졌음. (DX9·DX11 경험은 있으나 최신 명시적 API는 직접 구현하지 않음)
  [해결] 핵심 계약만 순수 가상으로 강제하고, 필수가 아닌 기능은 기본 구현을 둔 비-순수 가상(질의 훅·옵션 메서드)으로 두어, 인터페이스 변경 시 전 백엔드 동시 수정 부담을 줄임.
- [문제] 기능을 추가할 때마다 그것이 RHI(하드웨어 추상) 개념인지 상위 레이어(렌더러·씬) 개념인지 경계가 모호한 추상화 수준 딜레마에 반복해 부딪힘.
  [해결] 기능마다 "상위 레이어의 개념인가"를 판별 기준으로 삼아 씬·렌더링 의미는 상위로 올리고, RHI는 얇은 계층으로 유지.
- [문제] 초기에 플랫폼 헤더(`Windows.h`·`vulkan.h`) 오염이 상위 모듈로 새어 빌드가 깨지거나 바이너리가 비대해지는 문제를 확인.
  [해결] 빌드·링크 결과(깨짐·비대)를 팀과 공유한 뒤, Pimpl 은닉을 적용하도록 백엔드 담당에 주문하여 플랫폼 헤더를 RHI 경계 안에 가둠.
- [문제] 방향과 아키텍처를 한 사람이 끌면 본인이 막힐 때 팀 전체가 멈추는 단일 병목과 기여 의존이 생김.
  [해결] 모듈 경계를 POD·인덱스(ID) 통신으로 고정하여, 팀원이 인터페이스만 맞추면 독립적으로 병렬 진행하도록 함.
- [문제] 셰이딩·메시(Light·Shadow·Model·Mesh, 팀원 담당)가 본인이 만든 렌더러 골격 위에서 동작해야 하므로 타인의 산출물을 받아 통합해야 했음.
  [해결] Renderer와 `IRenderPath`가 팀원 산출물을 씬·머티리얼 데이터로 받도록 인터페이스화하여 통합 지점을 단순화.

---

# Ⅵ. 참고자료
- Acton, M. (2014). *Data-Oriented Design and C++*. CppCon 2014.
- Albrecht, T. (2009). *Pitfalls of Object-Oriented Programming*. Game Connect: Asia Pacific (GCAP) 2009, Sony Computer Entertainment. https://www.gamedevs.org/uploads/pitfalls-of-object-oriented-programming.pdf
- Drepper, U. (2007). *What Every Programmer Should Know About Memory*. Red Hat. https://people.freebsd.org/~lstewart/articles/cpumemory.pdf
- Nikolov, S. (2018). *OOP Is Dead, Long Live Data-Oriented Design* [Video]. CppCon 2018.
