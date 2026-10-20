# performance 최적화 문제 목록

- 관측일: 2026-10-19
- 관측 시점: `performance` / `9437dd95b5cf`
- 조사 범위: dyf, 공통 RHI, Vulkan·D3D12·Metal 및 실제 렌더 경로에 연결된 Model·AdvancedRendering
- 목표: Bistro의 안정적인 60 FPS와 Android·iOS에서 감당할 수 있는 CPU 시간·GPU 시간·메모리 사용량
- 비교 기준: **현재 performance 구현에서 각 최적화 적용 직전과 적용 직후를 비교한다. main을 성능 기준으로 삼거나 과거 구현으로 원복하는 작업이 아니다.**
- 상태: 아래 관측표는 수정 전 정적 검토 결과다. 현재 작업 트리에 최적화 구현을 반영했으며 적용 범위는 진행 표에 기록했다. 실제 소요 시간·개선 효과는 미측정이고 빌드·native 실행은 수행하지 않았다.

## 진행 상태 — PERF-20261019

적용 전 기준은 `performance`의 `9437dd95b5cf`, 적용 후 구현은 `PERF-20261019` 변경으로 이 브랜치에 반영했다. **구현 적용 6개, 부분 적용 7개**이며 실측을 완료한 항목은 없다.

| ID | 상태 | 반영한 변경 | 남은 범위 |
| --- | --- | --- | --- |
| P01 | 구현 적용 | Metal draw·compute·tessellation 상수를 기존 업로드 풀의 256바이트 정렬 영역에 저장한다. 명령 완료 lease로 수명을 유지하고 상수별 GPU 버퍼 생성을 제거했다. | Apple 장치에서 native 검증·할당량·GPU 시간 측정 |
| P02 | 구현 적용 | D3D12 자원 보유 목록을 소유권을 가진 해시 맵으로 바꿔 선형 중복 검색을 제거했다. | Windows에서 native 검증·CPU 시간 측정 |
| P04 | 구현 적용 | 가시 불투명·알파 마스크 객체를 메시·동등 재질·LOD·현재/이전 정점 버퍼·면 방향·그림자 수신·변형 이력으로 묶어 CPU instanced draw를 구성한다. 엔티티 번호가 연속일 필요가 없다. 고유 메시지만 있는 씬은 정렬·재포장을 건너뛴다. | draw 수·CPU 준비 시간·그림 정확성 측정. 그림자는 기존 별도 경로(P21)를 유지한다. |
| P05 | 구현 적용 | Surface가 CPU 배치의 model·previous-MVP 스트림과 instanceCount를 사용한다. 대표 객체의 경계로 배치 전체를 다시 컬링하지 않는다. | 카메라·객체 이동, 변형 이력, TAA·모션의 native 검증 |
| P06 | 부분 적용 | 상수와 17개 draw·바인딩·dispatch·상태 명령의 인자를 명령 목록의 연속 바이트 저장소에 모았다. 재생 람다는 소유자와 offset만 보관하며 reset 시 저장소를 비운다. 빈 resource barrier 기록도 생략한다. | std::function 기반 재생 자체와 업로드·attachment·debug label 등 가변 길이 기록은 남아 있다. native 검증 필요 |
| P07 | 부분 적용 | 카메라 평면을 재사용하고 내부점 조기 수락·지지 정점 검사로 바꿨다. 큰 좌표의 상쇄 오차 여유를 유지한다. CPU instanced 객체는 준비 때 한 번 컬링하고 Surface·Main에서 재검사하지 않는다. | 비인스턴스 경로의 패스 간 가시 결과 공유, native 검증·실제 시간 측정 |
| P09 | 부분 적용 | GPU visibility의 객체·배치·카메라 바이트가 같으면 해당 업로드와 배리어를 생략한다. dirty 상태는 제출 성공 때만 해제한다. | CPU 배치·입력 구성 자체의 변경 추적·재사용 |
| P11 | 부분 적용 | 텍스처 identity 인덱스를 프레임 간 유지해 매 프레임 map node를 다시 만들지 않는다. | 재질 순회·바인딩 구성 캐시와 AdvancedRendering bindless 경로 |
| P12 | 구현 적용 | 텍스처 슬롯을 identity·색 공간으로 유지한다. 재질 순서 변경은 기존 GPU 텍스처를 재사용하고, 실제로 사용하지 않는 소유자만 해제한다. 빈 슬롯은 재사용한다. | 재질 변경·씬 전환·업로드 실패 후 재시도의 native 검증 |
| P13 | 부분 적용 | D3D12·Metal의 동일 자원·서브리소스 상태 요구를 기록 중 합친다. 제출 시 최초 요구와 배리어 검증은 유지한다. | 공통 RHI와 백엔드 사이의 나머지 중복 검증 비용 |
| P15 | 부분 적용 | HUD가 꺼져 있으면 빈 Profiler HUD 패스와 그에 따른 기록·배리어를 만들지 않는다. | RenderGraph 저장소·컴파일 결과와 작은 패스 기록 재사용 |
| P16 | 구현 적용 | D3D12 allocator·native command list를 완료된 제출에서만 풀로 반환하고 reset해서 재사용한다. 직접 native 사용이나 완료를 모르는 실패 경로는 재사용하지 않는다. 캐시는 최대 64개다. | Windows에서 완료·실패·장치 해제 경로와 native 메모리 측정 |
| P19 | 부분 적용 | Vulkan 제출·Present fence를 완료 확인 후 회수해 reset·재사용한다. 캐시는 최대 64개이며 종료 때 해제한다. | Present의 추가 제출과 acquire 대기 범위 최적화, native 검증 |

P03·P08·P10·P14·P17·P18·P20~P24, 로딩·기능 비용·계측 공백 항목은 추가 작업 대상이다. 특히 모바일 GPU 압축 텍스처(P08)와 실제 Bistro 60 FPS 검증은 남아 있다.

### 검증 기록

| 검증 | 결과 | 범위·한계 |
| --- | --- | --- |
| 컬링 수학 교차 검산 | 고정 seed `20261019`의 20,000개 입력을 기존 8개 꼭짓점 판정과 비교했다. 기존 판정이 유지한 객체를 새 판정이 제거한 사례는 없었고, 경계 사례 2개는 더 보수적으로 유지했다. | Python 수학식 검산이다. perspective·orthographic·반전 축·회전·비균일 scale·mirror·shear·최대 1e12 translation·퇴화 경계를 포함한다. 실제 C++ 실행 검증을 대신하지 않는다. |
| 소스 구조 검토 | 변경 소스의 괄호 균형, 17개 기록 대상 native method의 선언, Metal의 상수별 newBufferWithBytes 제거, Public 변경 기록 일치를 확인했다. | 컴파일·링크·native API 검증이 아니다. |
| 자원 수명 검토 | CPU 배치 버퍼의 제출 성공/실패 transaction, 명령 payload의 reset·재생 수명, Metal 완료 lease, D3D12 완료 후 pool 반환, Vulkan fence 완료·reset·종료 해제 경로를 검토했다. | 장치별 실행 검증은 남아 있다. |
| Public 변경 | ICommandList.h의 private 저장소와 RenderExtension.h의 instanceCount를 [public_changed.txt](public_changed.txt)에 기록했다. | 기존 바이너리와의 ABI 호환성은 보장하지 않는다. 엔진·사용 코드 재빌드가 필요하다. |
| 빌드·native 실행·FPS | 미수행·미측정 | Bistro 60 FPS 달성이나 특정 ms 개선을 확인했다고 표시하지 않는다. |

### 구현의 영향

- CPU 배치는 가시 instance의 model·previous-MVP를 보관하는 버퍼와 CPU 캐시를 사용한다. 그림자나 기존 투명 draw에 필요한 canonical stream은 유지하며, 필요 없는 경우에는 만들지 않는다. 버퍼 용량은 증가 시 재사용을 위해 확장한다.
- 불투명·알파 마스크 draw는 배치 키 순서로 기록된다. 투명 draw의 정렬은 유지한다. coplanar 표면의 깊이 동률, 변형 이력, 옵션 전환은 native 화면 검증 대상이다.
- 품질 설정·에셋 해상도·host loop를 변경하지 않았다. 예제 전용 엔진 진입점을 추가하지 않았다.

## 읽는 기준

| 표시 | 의미 |
| --- | --- |
| 우선 검토 | draw 수·할당 수·메모리 사용량과 직접 연결되어 먼저 최적화를 검토할 항목. 실제 소요 시간 순위는 아직 미정이다. |
| 추가 검토 | 조건에 따라 비용이 커지는 경로와 구조적 한계. 해당 기능이 활성화된 조건에서 측정한다. |
| 기능 비용 | 의도된 렌더 기능의 비용. 같은 품질에서 가능한 최적화와 품질 조정에 따른 절감을 구분한다. |
| 계측 공백 | 성능 판단에 필요한 수치가 빠지거나 측정 경로 자체가 결과에 영향을 주는 경우. |

아래 관측표와 개선 방향은 최초 조사 내용이다. 구현 적용 여부와 남은 범위는 위 진행 표를 따른다. 모든 항목의 효과는 아직 미측정이며, 근거 링크의 줄 번호는 최초 관측 시점 기준이다.

## 1. 우선 검토

| ID | 관측한 문제 | 비용이 커지는 조건 | 최적화 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| P01 | Metal의 inline constants 설정마다 `newBufferWithBytes`로 GPU 버퍼를 생성한다. compute dispatch에서도 상수 버퍼를 생성한다. | draw·dispatch 수 증가, 특히 iOS | 완료 시점이 보장되는 상수 업로드 영역을 재사용하고 작은 상수의 전달 방식을 검토한다. | [draw 상수](src/Backends/Metal/MetalCommandList.mm#L1074), [compute 상수](src/Backends/Metal/MetalCommandList.mm#L804) |
| P02 | D3D12의 자원 보유 목록에 추가할 때마다 기존 목록 전체를 선형 검색한다. 고유 자원이 늘면 누적 비용이 O(N²)로 커질 수 있다. | 고유 버퍼·텍스처·heap이 많은 명령 목록 | 보유 순서와 수명을 유지하면서 중복 검색을 상수 시간에 가깝게 줄인다. | [RetainObject](src/Backends/D3D12/D3D12CommandList.cpp#L61) |
| P03 | D3D12는 ResourceSet마다 별도 shader-visible descriptor heap을 생성하고, 바인딩할 때 heap과 root descriptor table을 설정한다. | 재질 전환, ResourceSet 증가·교체 | 공용 descriptor 영역과 세트별 할당·수명 관리, 중복 바인딩 생략을 검토한다. | [heap 생성](src/Backends/D3D12/D3D12Device.cpp#L2346), [바인딩](src/Backends/D3D12/D3D12CommandList.cpp#L1057) |
| P04 | CPU instancing은 엔티티 번호와 입력 순서가 연속인 객체만 묶는다. `A, B, A`의 두 A는 별도 draw가 된다. | 반복 메시가 씬 안에 흩어져 있는 경우 | 불투명·알파 마스크 draw의 배치 키와 인스턴스 목록을 구성한다. 투명 순서·LOD·반전 변환 등 기존 렌더 계약을 보존한다. | [MainForward 묶기](src/dyf/ForwardPass.cpp#L205) |
| P05 | CPU 경로의 Surface 패스는 객체마다 `instanceCount=1`로 그린다. MainForward의 CPU instancing이 이 패스로 이어지지 않는다. | 반복 객체와 Surface 패스 동시 사용 | Surface의 모션·이전 변형 입력을 유지하면서 CPU instancing을 연결한다. | [Surface draw](src/Extends/AdvancedRendering/ScreenSpaceLighting.cpp#L565) |
| P06 | 공통 RHI는 명령을 `std::function`으로 저장한 뒤 native 명령으로 다시 재생한다. inline constants마다 별도 `vector`를 할당·복사한다. | draw 수 증가 | 연속 명령 저장소·상수 저장 영역 재사용을 검토한다. 호출 이후 입력 데이터의 수명 보장과 기존 검증을 유지한다. | [상수 복사](src/RHI/ICommandList.cpp#L831), [native 재생](src/RHI/Resources.cpp#L577) |
| P07 | CPU frustum 검사는 호출마다 view-projection을 계산하고 AABB의 최대 8개 꼭짓점을 double 연산으로 변환한다. Surface와 MainForward가 같은 객체를 각각 검사한다. | CPU culling, 대형 씬, 여러 geometry 패스 | 프레임 공통 frustum·객체 경계·가시 결과를 재사용한다. 큰 좌표·shear 등 기존의 보수적 판정 조건을 유지한다. | [검사 구현](src/dyf/SceneAlgorithms.h#L42), [MainForward](src/dyf/ForwardPass.cpp#L201), [Surface](src/Extends/AdvancedRendering/ScreenSpaceLighting.cpp#L398) |
| P08 | DDS의 압축 데이터와 기존 mip을 유지하지 않고 mip 0을 RGBA로 디코딩한 뒤 mip을 재생성한다. GPU 업로드도 RGBA8이며 CPU 이미지도 보유한다. | Bistro의 대형 텍스처, 모바일 메모리·대역폭 | 플랫폼별 GPU 압축 포맷과 준비된 mip 전달 경로, CPU 이미지 보유 정책을 검토한다. 4096² RGBA8 하나의 전체 mip 크기는 약 85 MiB다. | [DDS 처리](src/dyf/Image.cpp#L59), [업로드 포맷](src/dyf/TextureCache.cpp#L126), [RHI 포맷](src/Public/dyf/RHI/Format.h#L7) |

## 2. 추가 검토 — dyf·공통 RHI

| ID | 관측한 문제 | 영향 조건 | 최적화 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| P09 | GPU culling을 켜도 매 프레임 CPU에서 그룹·재질 대표·객체 경계를 다시 구성하고, 객체당 192바이트 입력과 배치 데이터를 전체 업로드한다. | GPU visibility 사용 | 정적 geometry·재질·배치 정보와 프레임별 카메라·모션 정보를 분리하고 변경 범위만 갱신한다. | [CPU 배치 구성](src/dyf/GpuScene.cpp#L26), [전체 업로드](src/dyf/GpuVisibility.cpp#L146), [입력 크기](src/dyf/GpuVisibility.h#L104) |
| P10 | GPU visibility의 모든 배치·LOD에 대해 CPU가 indirect draw를 발행한다. 해당 프레임에 instance count가 0이어도 호출 자체는 남는다. | 많은 배치·LOD | 배치 수와 CPU 호출 비용을 먼저 측정하고 간접 명령 압축·다중 draw 지원을 검토한다. | [배치별 draw](src/dyf/ForwardPass.cpp#L127) |
| P11 | 텍스처 목록·재질 상태·바인딩을 매 프레임 순회·구성한다. ResourceSet 캐시는 생성 비용을 줄이지만 구성·검색은 남으며, AdvancedRendering에서는 bindless 경로가 비활성화된다. | 재질이 많은 씬 | 변경 추적과 재질 바인딩 재사용, AdvancedRendering의 바인딩 경로를 검토한다. | [텍스처 순회](src/dyf/TextureCache.cpp#L28), [세트 구성](src/dyf/MaterialBindings.cpp#L57), [bindless 조건](src/dyf/Renderer.cpp#L545) |
| P12 | 텍스처 캐시 슬롯이 텍스처의 발견 순서에 연결돼 있다. 앞쪽 재질의 텍스처 변경으로 순서가 밀리면 뒤쪽 슬롯도 이미지가 바뀐 것으로 판단할 수 있다. | 재질·텍스처 편집과 씬 전환 | 이미지 identity·색 공간을 기준으로 안정적인 캐시를 유지하고 실제로 바뀐 항목만 업로드한다. | [슬롯 갱신](src/dyf/TextureCache.cpp#L44) |
| P13 | 공통 RHI의 상태 검증에 더해 백엔드에서도 바인딩과 제출 상태를 검증한다. D3D12·Metal은 동일 상태 요구도 operation 목록에 반복 추가한다. | 바인딩·서브리소스 수 증가 | 검증 범위를 명확히 하고 배리어 사이의 동일 상태 요구를 합친다. 자원 수명·상태 오류 검출은 유지한다. | [공통 검증](src/RHI/ICommandList.cpp#L484), [D3D12](src/Backends/D3D12/D3D12CommandList.cpp#L216), [Metal](src/Backends/Metal/MetalCommandList.mm#L212) |
| P14 | 자원 생성·업로드 기록이 공통 device mutex를 사용한다. 병렬 기록은 패스 단위이며 큰 MainForward 내부의 객체 순회를 worker가 나누지 않는다. | worker 증가, 대형 단일 geometry 패스 | 잠금 대기 시간을 측정하고 자원 준비의 직렬 구간과 geometry 기록 분할을 검토한다. | [업로드 잠금](src/RHI/Resources.cpp#L876), [패스별 기록](src/RHI/RenderGraph.cpp#L437) |
| P15 | 매 프레임 RenderGraph를 새로 구성하고 자원 등록·의존성·배리어를 계산한다. 병렬 모드는 패스별 명령 목록을 추가한다. | 많은 패스·등록 자원 | 그래프 저장소·컴파일 결과 재사용과 작은 패스의 명령 목록 묶기를 검토한다. | [프레임 그래프](src/dyf/SceneRendering.cpp#L221), [Compile](src/RHI/RenderGraph.cpp#L175), [병렬 명령 목록](src/RHI/RenderGraph.cpp#L414) |
| P21 | 그림자 atlas 재사용 판정에 전체 caster 상태를 매 프레임 직렬화·비교한다. 다시 그릴 때 shadow view마다 전체 엔티티를 순회하며 원본 LOD를 사용한다. | 많은 caster·그림자 view, 이동 카메라·객체 | caster 변경 추적과 view별 후보 목록을 검토한다. 그림자 LOD는 실루엣·atlas 재사용에 미치는 영향을 따로 평가한다. | [캐시 검사](src/dyf/Lighting.cpp#L253), [view별 순회](src/dyf/ShadowPass.cpp#L152), [원본 LOD](src/dyf/ShadowPass.cpp#L89) |
| P24 | 그림자·HDR·instancing·wireframe 등의 설정 변경이 파이프라인·연관 자원 재생성으로 이어진다. 일반 설정 변경도 temporal history를 무효화한다. | 시연·전시의 옵션 전환 | 설정별 실제 의존 자원만 갱신하고 필요한 경우에만 history를 무효화한다. 전환 직후 hitch와 정상 상태 FPS를 구분한다. | [설정 적용](src/dyf/Renderer.cpp#L233) |

## 3. 추가 검토 — Backends

| ID | 관측한 문제 | 영향 조건 | 최적화 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| P16 | D3D12 native 명령 목록 획득 때마다 command allocator와 command list를 생성한다. 완료된 목록을 재사용하는 경로가 없다. | 매 프레임 다수 명령 목록 | fence 완료 이후 allocator·list를 재사용한다. | [생성](src/Backends/D3D12/D3D12CommandList.cpp#L317), [획득](src/Backends/D3D12/D3D12Device.cpp#L1030) |
| P17 | Vulkan ResourceSet마다 `maxSets=1`인 descriptor pool을 생성한다. | ResourceSet 캐시 미스·자원 교체 | 공유 pool과 완료 시점에 따른 descriptor 수명 관리를 검토한다. | [pool 생성](src/Backends/Vulkan/VulkanPipeline.cpp#L833) |
| P18 | Vulkan은 버퍼·이미지마다 별도 `vkAllocateMemory`를 사용한다. D3D12도 committed resource 방식이다. | 다수의 소형 자원, 자원 교체 | GPU 메모리 하위 할당과 재사용을 검토한다. 기존 staging upload page 재사용과는 별개인 항목이다. | [Vulkan 버퍼](src/Backends/Vulkan/VulkanResources.cpp#L146), [Vulkan 이미지](src/Backends/Vulkan/VulkanResources.cpp#L222), [D3D12 버퍼](src/Backends/D3D12/D3D12Buffer.cpp#L49) |
| P19 | Vulkan 제출마다 fence를 생성·파괴한다. Present에서도 별도 제출·fence를 추가하며 swapchain 획득 대기 stage는 `ALL_COMMANDS`다. | 프레임당 제출 증가 | fence 재사용·제출 묶기와 실제 backbuffer 최초 접근에 맞는 대기 범위를 검토한다. 표시와 자원 완료 보장은 유지한다. | [제출](src/Backends/Vulkan/VulkanDevice.cpp#L1080), [Present](src/Backends/Vulkan/VulkanDevice.cpp#L1126), [fence 회수](src/Backends/Vulkan/VulkanDevice.cpp#L1993) |
| P20 | Metal은 compute dispatch마다 encoder를 열고 닫는다. | skinning·HiZ 등의 작은 dispatch 연속 실행 | encoder 경계 비용을 측정하고 hazard 보장을 유지하는 범위에서 묶기를 검토한다. | [encoder 생성](src/Backends/Metal/MetalCommandList.mm#L786), [encoder 종료](src/Backends/Metal/MetalCommandList.mm#L828) |

## 4. 추가 검토 — Model

| ID | 관측한 문제 | 영향 조건 | 최적화 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| P22 | compute skinning을 사용해도 정확한 bounds를 위해 CPU가 모든 정점을 skinning한다. 현재 position만 계산하도록 줄였지만 O(정점 수) 순회는 남아 있다. influence 전체 바이트 비교도 반복한다. | skinned mesh, 다수의 애니메이션 instance | 보수적 joint bounds·GPU bounds·변경 revision을 검토한다. culling 오류와 변형 이력 손실을 피한다. | [bounds 순회](src/Extends/Model/ModelRenderer.cpp#L668), [influence 비교](src/Extends/Model/ModelRenderer.cpp#L403) |
| P23 | `UpdateAnimations` 호출 시 정지 모델도 기본 pose·전역 행렬·바인딩을 재평가한다. | 정지·정적 instance에도 매 프레임 호출하는 경우 | pose·루트 변환·morph·재생 상태의 변경을 추적해 필요한 instance만 갱신한다. | [애니메이션 갱신](src/Extends/Model/ModelScene.cpp#L125) |

## 5. 로딩·첫 프레임·캡처 비용

정상 상태의 지속적인 프레임 비용과 별도로 측정한다.

| ID | 관측한 비용 | 발생 시점 | 최적화 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| L01 | 텍스처 mip을 CPU에서 생성하고 이미지마다 제출 후 완료를 기다린다. mip 준비 worker를 늘려도 GPU 업로드 대기는 텍스처 단위다. | 최초 업로드·새 텍스처 적용 | 메모리 상한을 유지하는 업로드 묶기·비동기 완료와 준비된 mip 사용을 검토한다. | [mip 생성](src/dyf/TextureCache.cpp#L81), [제출·대기](src/dyf/TextureCache.cpp#L171) |
| L02 | 준비된 LOD가 없으면 렌더 진입 중 CPU에서 메시 단순화를 수행한다. | 메시 최초 준비·LOD 활성화 | 에셋 준비 단계로 옮기거나 결과를 캐시한다. | [LOD 준비](src/dyf/GeometryUpload.cpp#L102), [단순화](src/dyf/Lod.cpp#L15) |
| L03 | 환경맵의 SH·GGX prefilter와 BRDF LUT를 CPU에서 생성한다. prefilter는 mip별 64 samples, LUT는 128² 픽셀당 128 samples를 사용한다. | 환경 최초 준비·환경 소스 교체 | 준비된 결과·공용 LUT 캐시와 GPU 사전 계산 경로를 검토한다. | [prefilter](src/Extends/AdvancedRendering/EnvironmentLighting.cpp#L161), [환경 생성](src/Extends/AdvancedRendering/EnvironmentLighting.cpp#L327) |
| L04 | Vulkan texture readback은 queue idle을 기다린다. 매 프레임 캡처하면 렌더 성능에 동기화 비용이 섞인다. | readback 요청·프레임 저장·환경 캡처 | 지연 readback·완료 추적을 검토하고 캡처 없는 성능도 별도로 측정한다. | [readback 대기](src/Backends/Vulkan/VulkanDevice.cpp#L1363) |

## 6. 기능 자체의 GPU·메모리 비용

아래 항목은 기능이 수행하는 작업량이다. 불필요한 중복 작업의 제거와 품질 변경에 따른 절감을 구분해서 기록한다.

| ID | 관측한 비용 | 측정할 조건 | 검토 방향 | 근거 |
| --- | --- | --- | --- | --- |
| Q01 | Surface normal·albedo·motion attachment 3개가 full-resolution RGBA16F다. depth와 HDR 출력·후처리 이력은 별도다. | 모바일 해상도, Surface 사용 | 채널별 정밀도·packing·attachment 사용량과 저장·읽기 대역폭을 검토한다. | [타깃 생성](src/Extends/AdvancedRendering/ScreenSpaceLighting.cpp#L354), [Surface attachment](src/Extends/AdvancedRendering/ScreenSpaceLighting.cpp#L553) |
| Q02 | GI는 평가 픽셀당 최대 4개 ray × 16 steps, SSR은 최대 32 steps와 교차 정밀화·후속 denoise를 수행한다. | GI·SSR 각각 및 동시 사용 | 패스별 GPU 시간·이력 메모리와 해상도별 비용을 확인한다. 품질을 바꾼 결과는 같은 품질 최적화와 구분한다. | [GI·SSR 추적](src/Extends/AdvancedRendering/Shaders/screen_space_ps.hlsl#L101), [denoise 이력](src/Extends/AdvancedRendering/ScreenSpaceLighting.cpp#L90) |

## 7. 계측 공백

| ID | 관측한 한계 | 판단에 미치는 영향 | 보완 방향 | 근거 |
| --- | --- | --- | --- | --- |
| M01 | RenderToTexture 경로에서는 일반 프레임 GPU query와 timing 집계를 건너뛴다. | 통합 GUI의 offscreen 씬을 기존 Profiler 수치만으로 평가하기 어렵다. | offscreen 렌더 구간과 최종 GUI 합성 비용을 함께 관측할 수 있게 한다. | [GPU query 조건](src/dyf/SceneRendering.cpp#L337), [프레임 집계 조건](src/dyf/SceneRendering.cpp#L396) |
| M02 | Metal 백엔드는 `Feature::TimestampQuery`를 지원하지 않는다. | iOS·macOS에서 기존 Profiler로 GPU 패스 시간을 비교할 수 없다. | 실제 장치 지원에 맞는 GPU 시간 수집을 검토한다. 미지원 수치를 0 ms로 해석하지 않는다. | [기능 지원 판정](src/Backends/Metal/MetalDevice.mm#L383) |

## 8. 최적화 진행 기준

1. 현재 `performance`의 기능·렌더 결과를 유지하면서 최적화한다. 각 변경 직전의 상태를 해당 변경의 비교 기준으로 기록한다.
2. Bistro의 씬·카메라 경로·해상도·렌더 옵션·플랫폼·빌드 설정·계측 설정을 고정한다. 60 FPS 목표의 프레임 예산은 약 16.67 ms다.
3. 로딩, 옵션 전환 직후, 정상 상태를 분리한다. CPU 준비·기록·제출, GPU 패스 시간, draw·instance 수, 할당 수, 메모리를 함께 비교한다. 평균과 p95·p99 프레임 시간을 구분한다.
4. 우선 순서는 P04·P05의 draw 묶기, P06의 공통 RHI 할당, P01의 Metal 상수, P02·P03의 D3D12 자원·descriptor, P08의 모바일 텍스처 경로다. 실제 측정 결과에 따라 조정한다.
5. 예제 visualizer 전용 동작을 위해 엔진 Public API를 추가하지 않는다. Public 내용이 바뀌면 API 변경 여부와 무관하게 [public_changed.txt](public_changed.txt)에 파일·내용·이유·호환성 영향을 함께 기록한다.
6. 항목을 해결하면 변경 식별자, 검증 조건·결과, 측정 전후 수치를 해당 항목에 연결한다. 코드 수정만으로 효과를 확인했다고 표시하지 않는다.
