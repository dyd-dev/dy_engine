# Cinematic Showcase 영상 기획과 구현 계획

발표의 주제는 **빛·규모·반복이라는 서로 다른 렌더링 비용을 공통 렌더러가 어떻게 다루는가**다. 아름다운 화면을 먼저 보여 주고, 그 화면을 만드는 이론과 기술을 설명한 뒤, 실제 비용을 측정하고 불필요한 일을 줄이는 과정을 보여 준다. 각 장면은 **화면 → 이론 → 패스/데이터 → 측정 질문 → 한 가지 최적화의 증거**로 연결한다.

Bistro는 빛과 재질, **NVIDIA Emerald Square Day는 광역 가시성과 거리별 LOD**, Dense Arena는 많은 반복 오브젝트와 CPU 제출 비용을 담당한다. Emerald는 기존 로더가 지원하는 원본 FBX를 그대로 사용한다. 기능 토글을 차례로 누르는 영상보다 각 장면에서 하나의 질문에 답하는 구성을 우선한다. Sponza는 개발 회귀·효과 비교용, Jungle Ruins는 원본 보관 후보로 남기고 주영상에서는 제외한다.

최종 목표는 Windows_DX(D3D12), Windows_VK(Vulkan), Android Vulkan, iOS Metal에서 같은 장면·동선을 보여 주는 약 5분 영상이다. **Android/iOS는 별도 브랜치에서 진행한다.** 현재 개발 환경은 Linux Wayland/Vulkan이며 셰이더 소스, 플랫폼 빌드, 실제 기기 실행을 구분한다. Ray Tracing 24–28은 제외하고 advanced 01–23은 구현 목표로 유지하되, 모든 독립 예제를 5분 안에 하나씩 설명하지는 않는다. Mesh shader는 선택 경로이고 기본 기하 경로는 indexed draw다.

실행 예제는 [Cinematic Showcase](cinematic/README.md)와 [Dense Arena](arena/README.md)다. 효과·프로파일링·프레임 구성은 공통 dyf Renderer와 공개 RHI를 사용하며 backend는 네이티브 명령으로 번역한다. A/B·Dense Arena에 이어 실내 local probe·유리의 직접광 차폐·외부 식생 움직임을 연결했고 두 실행 파일의 Linux Wayland/Vulkan 빌드를 완료했다. 프로그램 실행·실제 화질·FPS·A/B 성능 수집은 이번 변경에서 수행하지 않았다. 기존 Bistro 실행 화면 확인과 이번 변경의 실행 확인을 섞어서 표기하지 않는다.

공유 glTF/FBX canonical geometry·공통 indexed LOD·Emerald 인자와 동선까지 포함한 `ShowcaseCinematic`·`ShowcaseArena` 두 target의 Linux Wayland/Vulkan 최종 빌드를 완료했다. 실행·구도·화질·성능 실측은 하지 않았다. 포맷 변환 경로는 채택하지 않으며 원본 FBX 광역 에셋으로 Emerald를 선정했다. Jungle은 원본 추출·환경 HDR 준비까지만 보관하고 Blender/USD 지원과 시연 통합은 추진하지 않는다.

## 5분 영상 구성

| 시간 | 화면과 핵심 설명 | 측정 질문 | 한 가지 최적화의 증거 |
| --- | --- | --- | --- |
| 0:00–0:20 | 세 장면의 가장 좋은 완성 컷. “여러 플랫폼에서 빛·규모·반복을 같은 렌더러로 다룬다.” | 목표 프레임 예산을 먼저 제시한다. 60 FPS는 약 16.67ms, 30 FPS는 약 33.33ms이며 달성 결과와 구분한다. | 이후 비교에서 해상도·화질·장면을 유지한다는 기준만 제시한다. |
| 0:20–1:35 | 잎·꽃이 움직이는 Bistro 외부와 Wine 실내. PBR은 재질의 반응, 조명·반사는 재질에 도착하는 빛이다. Surface, 실내 probe의 반사·확산광, 유리 차폐를 필요한 순간만 분해한다. | 정적 실내 조명은 무엇을 미리 계산하고 재사용할 수 있는가? | Wine의 정적 point shadow cache OFF/ON. 같은 화면에서 SHADOW CACHED·shadow draw 수·GPU 패스 시간·CPU 작업을 비교한다. probe 초기 준비 비용은 정상 프레임 측정과 분리한다. |
| 1:35–2:35 | Emerald Square Day의 거리 근경에서 건물·식생이 멀리 보이는 조감 구도로 이동한다. 원본 FBX 카메라와 마지막 광역 구도로 필요한 geometry의 변화를 보여 준다. | 지금 화면에 필요한 물체와 삼각형은 얼마나 되는가? | 같은 시각·구도에서 indexed LOD OFF/ON을 수집해 LOD0~3 분포·원본 대비 제출 triangles·관련 GPU 시간을 제시한다. 구도·실제 감소량·프레임 비용은 실행 후 수집한다. |
| 2:35–3:45 | Dense Arena의 여러 방에서 적·탄환·파편이 움직인다. 같은 mesh/material의 제출을 묶는 instancing을 설명한다. | 물체 수가 같을 때 CPU 제출/기록 비용과 GPU 작업량은 어떻게 달라지는가? | 같은 게임 시각·카메라에서 instancing OFF/ON. main draw 수 감소와 CPU 작업 변화를 측정하고, instance·triangle 수가 유지되는지 함께 본다. |
| 3:45–4:35 | Windows_DX/Windows_VK와 Android/iOS에서 같은 컷·게임 상태. 필요한 몇 초만 G-buffer·LOD를 표시한다. | 같은 세계가 기기별 프레임·메모리 예산에 들어가는가? | 실제 플랫폼명·GPU·해상도·품질 설정을 명시한다. 서로 다른 하드웨어의 수치를 API 자체의 우열로 해석하지 않는다. |
| 4:35–5:00 | 설명을 걷고 완성 화면으로 돌아온다. 장면별 측정 결과 한 줄과 에셋 크레딧으로 끝낸다. | 어떤 화면을 만들었고 어떤 중복 작업을 줄였는가? | 실제 수집한 A/B 결과만 최종 표에 넣는다. |

각 장면의 완성 화면을 주인공으로 두고 G-buffer, 패스 시간, 수치는 설명에 필요한 순간만 겹친다. 모든 효과의 ON/OFF를 넣지 않는다. 비교와 수집 대기 구간은 편집해 줄일 수 있지만 조건과 결과는 보존한다. 각 실행 파일이 위 장면을 자동으로 이어 붙이는 것은 아니다.

## 이론과 기술이 연결되는 방식

| 장면 | 이론 | 실제 기술과 데이터 | 관객에게 남길 설명 |
| --- | --- | --- | --- |
| Bistro | 렌더링 방정식의 실시간 근사, 미세면 BRDF·Fresnel·굴절·흡수, 가시성 | PBR, direct light와 shadow atlas, 외부 HDRI, 정적 실내 radiance capture·diffuse SH·box-projected reflection, Surface·SSAO/SSGI/SSR·유리·TAA/HDR | 재질 수식과 그 표면에 들어오는 빛은 별개의 문제다. 화면 밖 정보는 정적 probe로 보완하지만 한 위치의 캡처로 공간 전체를 정확히 복원하지는 못한다. |
| Emerald Square | 가시성 판정, 화면 오차에 따른 detail 선택 | 원본 FBX의 공유 geometry·노드별 transform, frustum culling, 픽셀 오차·hysteresis 기반 indexed LOD; 이후 GPU culling·indirect | 반복 geometry는 공유하고 원거리 디테일은 화면 오차 예산에 맞춘다. 실제 geometry·시간 변화는 같은 카메라 상태로 측정한다. |
| Dense Arena | 배치 처리와 자원 재사용, CPU/GPU 작업 분담 | 공유 quad·material, instance transform 갱신, CPU 컬링, RenderGraph 기록 | 같은 geometry의 반복 제출을 묶는다. 물체 수와 픽셀·정점 처리량까지 사라지는 것은 아니다. |
| 공통 플랫폼 | 명시적 자원 수명·상태·동기화 | RenderGraph 의존성, RHI 자원/명령, backend 번역, 프레임 완료 후 재사용 | 같은 렌더링 의도를 각 API에서 실행한다. 기기별 품질과 플랫폼 앱 통합은 별도 책임이다. |

실내 local probe와 직접광의 그림자 차폐를 소스에 연결했다. 초기 래스터 캡처를 재사용하는 방식이며 RT·완전 GI·다층 굴절은 아니다. 공간별 lightmap/probe volume·GI 전용 시간 필터·다층 굴절은 다음 품질 단계다. 많은 기능을 열거하는 대신 현재 화면이 어떤 정보를 사용하고 무엇이 부족한지 밝힌다.

## Profiling을 실제로 사용하는 방법

Profiling은 “느리다”를 “어떤 작업에 몇 ms를 쓰는가”로 바꾸는 과정이다. GPU 사용률이나 FPS 하나만으로 병목을 단정하지 않는다. F11로 공통 profiler를 열어 패스별 비용을 본다. A/B 수집은 큰 HUD를 자동으로 숨기고 같은 작은 진행 표시를 사용한다. GPU 계측은 계속하며 수집 뒤 F11로 다시 내역을 열 수 있다.

| 지표 | 읽는 방법 |
| --- | --- |
| FRAME 평균·MAX | 렌더 루프의 프레임 간격이다. 화면 표시 완료 FPS 측정과 같지 않다. 평균의 역수인 cadence도 같은 한계를 가진다. MAX는 수집한 집계 구간의 최대값이며 p95가 아니다. |
| CPU WORK | 렌더러의 prepare + record + submit 경과 시간이다. 전체 게임 업데이트나 모든 worker의 CPU 사용 시간을 합친 값이 아니다. |
| CPU ACQUIRE / PRESENT / READBACK | 프레임 확보·표시·읽기 단계이며 대기가 섞일 수 있다. 이 시간을 CPU 렌더 계산 비용으로 모두 더해 병목을 판단하지 않는다. |
| GPU SPAN | 프레임 데이터 업로드부터 마지막 그래프 barrier까지의 timestamp 범위다. HUD는 포함하고 최초 에셋 업로드·acquire·readback·present는 제외한다. |
| GPU 패스별 ms | Shadow·Surface·AO·환경광·Main·GI/SSR·투명·후처리 등 실제 완료한 패스의 시간이다. 기존 GPU MAIN을 전체 GPU 프레임으로 부르지 않는다. |
| Main/Shadow draws·instances·triangles, CPU-culled | 시간이 줄어든 이유를 설명하는 작업량이다. 배포 에셋의 전체 삼각형 수와 프레임 제출량은 다르다. |
| Main LOD0~3·원본 → 제출 triangles | 같은 Main 가시 인스턴스의 원본 LOD0 삼각형 수와 선택한 index buffer의 제출량이다. 원거리 근사로 줄인 geometry이며 Shadow는 LOD0를 유지한다. |

GPU timestamp는 완료된 샘플만 사용한다. 지원하지 않거나 아직 완료 샘플이 없으면 N/A이며 0ms 성능으로 취급하지 않는다. 비활성·캐시 재사용으로 실행하지 않은 패스도 0ms 측정 샘플을 만든 것으로 해석하지 않는다. CPU와 GPU는 겹쳐 실행될 수 있으므로 두 시간을 더한 값을 프레임 시간이라고 설명하지 않는다.

### A/B 공통 절차

1. 비교할 구도·해상도·화질을 정하고 F11 표시 여부, VSync, 컬링, worker 수 등 비교 대상 외 조건을 고정한다.
2. F5로 기준 A를 시작한다. 선택한 최적화만 OFF가 되며 현재 장면·카메라를 정지한다.
3. 최소 1초이면서 10제출 프레임 이상 준비한 뒤, 최소 3초이면서 30제출 프레임 이상을 수집한다. 화면의 WARMUP/CAPTURE가 끝날 때까지 기다린다.
4. F6로 저장한 동일 상태에서 최적화 ON인 B를 수집한다. 기준 수집이 없거나 나머지 조건이 다르면 비교를 거절한다.
5. 화면 하단 A/B와 콘솔의 CPU 작업/대기·GPU span/패스·draw/instance/triangle을 함께 읽는다. F7은 취소·결과 삭제다.
6. 측정 후에도 정지 상태를 유지한다. Space로 재생한다. 카메라·설정·창 크기·초점이 바뀐 측정은 취소된다.

수집은 profiler가 발행한 snapshot을 sequence별 한 번 반영하고 각각의 frame/sample 수로 가중 평균한다. GPU와 패스는 각자의 완료 sample 수를 사용하며 draw 통계는 새 제출 프레임별로 집계한다. 원시 프레임 분포나 percentile을 저장하는 기능은 아니므로 p95·p99·장시간 안정성을 이 결과만으로 주장하지 않는다.

VSync는 기본적으로 켜져 있다. 표시 주기 제한 때문에 최적화 후 FRAME이 같아도 CPU WORK나 GPU SPAN은 줄 수 있다. A/B 중간에 VSync를 바꾸지 않는다. Dense Arena의 V는 VSync를 전환하며 바꾼 뒤에는 기준부터 다시 수집한다. 서로 다른 VSync·해상도·화질 결과를 같은 최적화의 전후로 붙이지 않는다.

### Bistro에서 비교하기

Cinematic Showcase의 F8은 정적 그림자 캐시 → 병렬 RenderGraph 기록 → instancing → indexed LOD 중 비교 항목을 고른다. F5는 해당 항목만 OFF, F6는 ON이다. Wine 기본 항목은 정적 그림자 캐시, Bistro 외부는 병렬 기록, Emerald는 `INDEXED LOD`다. 다른 지원 장면에서도 F8로 LOD 항목을 선택한다.

주영상의 대표 비교는 Wine의 고정 point shadow다. 캐시가 켜진 프레임의 SHADOW CACHED와 shadow draw 감소를 보고, GPU shadow 작업의 제거 및 관련 CPU 작업 변화를 읽는다. 캐시 OFF와 ON에서 조명·그림자 화질 설정과 준비된 probe는 같다. 실내에는 잎 움직임을 자동 추가하지 않아 고정 caster를 유지한다. 외부의 잎·꽃은 움직이고 방향광 CSM도 카메라에 따라 달라지므로 외부에서 같은 정적 캐시 효과를 주장하지 않는다. 외부 F5/F6은 바람 시각·상태까지 저장·복원·정지한다. 병렬 기록 비교는 CPU RECORD를 중심으로 읽고 async compute로 부르지 않는다.

LOD 비교는 F8로 `INDEXED LOD`를 고른 뒤 F5/F6으로 같은 시각·구도에서 수집한다. L은 실제 LOD ON/OFF, C는 단계별 색상 표시다. 색상 debug도 원래 alpha를 유지하며 최종 화질 비교는 C를 끈다. HUD의 LOD0~3 분포와 `mainOriginalTriangleCount` → `mainTriangleCount`, GPU geometry 패스 시간을 같이 읽는다. 픽셀 오차·hysteresis·해상도를 고정하며 LOD는 디테일을 근사한 절충이므로 같은 geometry의 중복 처리 제거와 구분한다. 삼각형이 줄어도 fill/lighting 병목이면 프레임 시간이 거의 그대로일 수 있다.

[ / ]는 화면 기준 LOD 허용 오차를 절반 / 두 배로 바꾼다. 범위는 0.125~16px, 기본은 1px이며 큰 값일수록 더 거친 LOD를 허용하는 품질 조절이다. 선택한 오차를 전후 비교 양쪽에서 유지한다.

### Dense Arena에서 비교하기

Dense Arena의 F5는 현재 게임 시각과 중앙 방/전체 보기 상태를 저장하고 instancing OFF로 정지한다. F6는 같은 게임 시각·카메라로 복귀해 instancing ON을 수집한다. O는 중앙 방 근경/전체 보기, R은 결정론적 초기 상태 재시작, Space는 게임·카메라 재생/정지다.

대표 증거는 같은 instance·triangle 수에서 main draw 수와 CPU WORK/RECORD/SUBMIT이 어떻게 바뀌는가다. GPU 시간이 그대로라면 GPU 셰이딩 비용까지 줄였다고 설명하지 않는다. CPU 컬링은 Space로 상태를 고정하고 4로 별도 수동 비교한다. F5/F6의 자동 비교 대상은 instancing이며 GPU culling이나 indirect 시연이 아니다.

### 측정 결과를 읽고 다음 행동 정하기

| 관찰 | 다음에 살필 부분 | 주장할 수 있는 범위 |
| --- | --- | --- |
| CPU PREPARE/RECORD가 크고 GPU 여유가 있다 | 가시성 목록·재질/descriptor 검색·배치·기록 분담 | 실제 줄어든 CPU 단계와 draw 변화 |
| GPU Shadow가 크다 | 정적 결과 재사용·광원별 갱신 조건·caster 범위 | 같은 화질에서 다시 계산하지 않은 작업 |
| GPU geometry 관련 비용과 제출 triangles가 크다 | 컬링 단위·공유 인스턴스·indexed LOD | 실제 제거한 비가시/원거리 geometry |
| AO·GI·SSR·투명·후처리가 크다 | 중복 texture fetch·중간 이미지·효과별 입력/필터 구조 | 같은 설정에서 제거한 중복 또는 명시한 품질 절충 |
| ACQUIRE/PRESENT가 크고 작업 시간이 작다 | VSync·프레임 완료 대기·큐 포화 여부 | 대기 시간을 확인했다는 사실. 원인 확정은 별도다. |

Indirect는 GPU가 작성한 draw 인자를 CPU readback 없이 소비할 수 있게 하는 수단이다. 컬링·LOD와 연결한 데이터 흐름이 있어야 그 효과를 설명할 수 있다. 단순히 indirect 호출로 바꿨다는 이유로 속도 향상을 약속하지 않는다.

## 현재 구현 상태

| 구분 | 현재 소스·에셋 상태 | 아직 완료하지 않은 것 |
| --- | --- | --- |
| Bistro 장면·카메라 | 외부/Wine 선택, 정규화, 원본 FBX Camera 체인을 매 프레임 평가하는 60초(1분) 투어, 외부 잎·꽃의 별도 바람 시계 | 변경 후 실행, 모션·그림자·motion 정합, 최종 동선과 끊김의 실제 개선 여부 |
| Bistro 재질·조명 | DDS·normal·roughness/metal·AO 채널 보정, 실내 점광원 4개, 외부 HDRI·노출, 초기 12 view의 실내 정적 radiance capture·diffuse SH | 실제 화질·준비 시간·FPS, 공간별 GI/차폐, 광도·감쇠·노출 완전 정합, 면광원 근사 |
| Surface·화면 효과 | 실제 mesh의 법선·albedo·roughness·metal·AO·depth·motion, SSAO·SSGI·SSR·TAA·bloom·filmic | 완전한 deferred 조명, GI 전용 denoise·시간 안정화 |
| 유리 | Fresnel·thin-wall 굴절·고정 두께 색 흡수·specular AA, local box-projected 실내 반사와 직접광 shadow PCF | 실제 반사·차폐 결과, 별실 차폐·동적 반사·다층 유리/액체 굴절 |
| 성능 경로 | AO guide 재사용, 중복 Blend 처리 제외, descriptor 검색 개선, 정적 point/spot shadow atlas 재사용, profiler/A-B 연결 | 개선 전후 실제 성능과 이번 변경의 실행 확인 |
| 공유 geometry·indexed LOD | glTF/FBX canonical geometry·독립 노드 transform, 원본 VB+최대 3개 추가 IB, 픽셀 오차 선택·hysteresis, L/C·HUD·LOD A/B 연결 | 실제 화질·생성 시간·메모리·FPS, GPU LOD 선택·indirect 소비 |
| Dense Arena | Kenney 기반 9개 방·고정 동적 풀·공유 geometry, CPU 컬링·instancing·자원 재사용, 동일 상태 A/B 소스 | 실제 게임 플레이/충돌, GPU culling·indirect·LOD 통합 |
| Emerald Square Day | 원본 FBX·HDR 다운로드/추출, emerald 인자·원본 Camera·60초 광역 동선과 LOD A/B 연결 | 실제 실행·재질/광원·구도·공유량·LOD 전환·촬영 |
| Jungle 보관 후보 | 약 7.44GB 원본 USD 묶음 추출·HDR 다운로드 완료. Blender/USD 미지원, 포맷 변환 경로 미채택 | 현재 영상 장면에서 제외하며 통합 완료로 표기하지 않음 |
| Indirect | 공통 RHI와 독립 17 예제에 GPU 작성 인자 실행 경로 | Bistro/Emerald/Dense Arena의 가시성·LOD 데이터와 연결 |
| 플랫폼 | Linux Wayland/Vulkan 개발 및 기존 실행 파일, GLSL/HLSL/Metal 소스 | Windows_DX/Windows_VK 산출물과 실행, 별도 Android/iOS 브랜치 앱·기기 촬영 |

토글은 비활성 효과의 실제 RenderGraph 패스를 제외한다. AO는 흰색, 비활성 환경광·반사는 검은색 입력을 사용하며 Surface 정보가 필요한 효과가 없으면 Surface prepass도 제외한다. V debug view는 실제 메시 표면 정보를 표시하고 jitter·시간 변화 seed를 멈춘다. 이는 완전한 deferred 구현과 구분한다.

## 장면과 에셋 준비

### Bistro: 빛과 재질

출처는 [NVIDIA ORCA Amazon Lumberyard Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)다. /home/suhan/dev/assets/Bistro_v5_2.zip(894,377,473B)을 내려받아 /home/suhan/dev/assets/Bistro_v5_2에 압축 해제했다. 번들 LICENSE.txt는 CC BY 4.0이다. 외부 약 283만·내부 약 105만·Wine 약 129만 삼각형은 배포본 전체 수치이며 실제 제출 통계와 구분한다.

카메라는 단위가 다른 legacy Position/Interest가 아니라 Camera 노드 원점·+X 전방·+Y 위쪽을 모델과 같은 계층·정규화 변환으로 계산한다. 장면은 최대 축 80단위, Y-up에서 Z-up으로 한 번 회전한다. 카메라·조상의 실제 움직이는 클립에서 해당 체인의 트랙만 보관하고, 매 프레임 60초(1분) 투어의 현재 시각을 원본 클립 시각으로 환산해 위치·회전·스케일 키와 체인 행렬을 평가한다. 원본 키의 보간 방식·부모 움직임·roll·컷을 유지하고 수직 FOV는 50도 고정이다. 종전의 301개 위치·시선·up 중간 샘플과 선형 재보간은 제거했다. FBX import 단계의 TRS bake·상수 키 정리·10,000,000개 저장 키 예산은 별도이며 유지한다. 이번 카메라 변경은 아직 실행하지 않았고 실제 끊김이나 렌더링 프레임 저하의 해결 여부는 미확인이다. 첫 업로드는 투어 시간에서 제외한다. 외부/내부는 별도 에셋이므로 출입문 통과를 약속하지 않고 컷으로 연결한다.

외부는 원본 노드 이름과 BaseColor 텍스처 이름을 함께 확인해 분리된 나무 잎·담쟁이 잎·꽃 부분에만 작은 rigid sway를 적용한다. 나무줄기·화분·건축물·유리·낙엽은 고정하며 잎별 vertex bending은 아니다. N은 기본 ON인 바람을 전환하고 Space·창 비활성화는 정지, R은 시각을 초기화한다. 카메라와 별도인 바람 시계는 자유 카메라나 투어 종료 후에도 흐르며 F5/F6에서는 저장·복원·동결한다. 실내는 정적 point shadow 캐시를 유지하기 위해 모션을 추가하지 않는다.

Specular DDS 201개의 R 채널은 모두 0이어서 Bistro 프리셋의 authored occlusionStrength를 0으로 두고 G/B roughness·metalness와 화면 공간 AO를 유지한다. DirectX normal의 Y 부호는 -1이다. .pyscene은 실행하지 않고 필요한 값을 typed override로 연결한다.

외부는 원본 환경광 intensity 10과 노출 0.04를 사용한다. Wine은 야외 HDRI·임의의 태양 없이 일반 BistroInterior.fbx의 점광원 4개와 원본 발광 보정 1000배, 노출 0.12를 사용한다. 위치·range는 정규화 scale, inverse-square intensity는 scale²로 변환한다. 이는 공통 렌더러 기본값 변경이 아니며 원본 렌더러와 광도·감쇠·노출이 완전히 일치한다는 보장도 아니다.

실내 시작 시 첫 원본 카메라 위치에서 한 면 256×256의 6방향 HDR 래스터 캡처 두 벌을 만들고 각각 1024×512 panorama로 저장한다. 총 12개 view 가운데 발광 포함 캡처는 거칠기별 반사용으로, 발광 제외 캡처는 diffuse SH용으로 사용한다. 작은 고휘도 램프가 diffuse SH를 과도하게 지배하는 경로를 줄이되 반사에서는 램프 모양을 유지하는 구성이다. opaque/masked geometry만 캡처하며 투명·후처리·다중 반사를 포함하지 않는다. 반사는 실제 geometry 월드 AABB로 위치를 보정하지만 diffuse SH는 한 위치의 값이고 별실 차폐·벽 너머 빛 누출·공간별 GI를 해결하지 않는다. 발광 geometry를 면광원처럼 적분하는 구현도 아니다. 초기 준비 후 매 프레임 캡처하지 않으며 준비 비용은 정상 프레임 A/B와 별도로 설명한다. 실내 SSGI는 직접광·probe 근사의 중복과 밝은 샘플 불안정·비용을 줄이기 위해 기본 OFF로 바꿨고 8키로 켤 수 있다. 이 설정 변경을 기존 품질 구성 그대로의 속도 향상으로 설명하지 않는다. 이번 변경의 실제 화질·준비 시간·FPS는 미확인이다.

Glass/GlassWine IOR 1.55, 액체 1.33, Ice 1.31을 사용한다. legacy diffuse tint는 투과색으로 쓰지 않고 Beer–Lambert 흡수로 색을 처리한다. 유리 3mm·액체 4cm·얼음 3cm는 정규화 길이에 맞춘 고정 두께 근사다. 양면을 가시 법선에 정렬한 air→IOR thin-wall 모델이며 nested volume 추적이 아니다. Wine 유리는 초기 local probe에서 실내 구조·가구·발광 표면을 반사하고 box projection으로 위치 차이를 근사한다. 직접광에는 기존 shadow atlas의 PCF 차폐를 적용한다. 투명 물체는 probe 캡처에 없고 유리·액체가 겹친 다층 굴절도 남아 있다.

흰 하이라이트의 시간 불안정을 줄이기 위해 GGX 정밀도와 specular AA를 수정했다. 잔여 흰 점이 모두 해결됐다고 확정하지 않는다. SSGI의 고분산 밝은 hit와 SSR HDR 샘플, 별도 GI 필터 부재는 추가 품질 과제다. 상세 구현·한계는 [실행 README](cinematic/README.md)에 유지한다.

고정 point/spot-only stock shadow는 성공 제출된 atlas를 재사용한다. 광원·caster·alpha 입력·atlas 변경 시 다시 그리고 카메라 이동만으로는 무효화하지 않는다. Wine의 4점광원은 24개 view가 대상이며 방향광·혼합광·custom 입력은 이 캐시 대상이 아니다. AO guide 재사용·Blend 중복 처리 제외·descriptor 검색 변경도 화질 설정을 낮추지 않는 최적화다. 효과 크기는 실측 전이다.

glTF 반복 primitive는 geometry를 한 번 디코딩하고 `ModelMesh::geometryIndex`로 이전 소유 항목을 참조한다. 노드별 transform·재질은 유지하며 `LoadModelAsset`은 같은 MeshID를 재사용한다. 공개 raw `ModelData`를 직접 읽는 코드는 geometry 참조를 resolve하고 node·skin·morph weight는 현재 항목에서 읽어야 한다. 일반 static geometry는 예전 world-baked 대신 node-local이며 skin·morph는 독립 경로, 정적 mirror는 기존 winding을 보존하는 baked 경로다. bounds와 merge 및 Legacy ModelAnim 소비자도 새 계약을 반영했다.

FBX도 같은 원본 mesh identity·geometry-to-node의 정확한 offset·실제 node material slot 배치가 일치하는 non-skin/non-morph 반복 배치를 canonical geometry로 공유한다. world transform은 노드별로 유지하고 offset·material override가 다르면 독립 디코딩한다. 정적 mirror·skin·morph는 기존 경로를 보존한다. 디코딩 예산은 공유 geometry 한 번과 배치별 메타데이터를 계산하며 광원 bounds에는 각 배치의 world 위치를 포함한다. 서로 다른 원본 mesh를 내용 비교로 합치는 기능은 아니다.

공통 indexed LOD는 immutable mesh별 최초 준비 때 `BuildMeshLods`를 CPU에서 한 번 실행하고 원본 VB와 최대 3개 추가 IB를 캐시에 보관한다. 기본값은 `enableLod=true`, `lodPixelError=1.0`, `lodHysteresis=0.15`, `debugLodColor=false`다. Main·Surface·유리는 같은 단계를 선택하고 Shadow는 LOD0다. 엄격한 경계·UV/법선 seam 보호 때문에 일부 mesh는 충분히 줄지 않거나 추가 단계가 없을 수 있다. 원본 vertex와 텍스처 저장은 유지되고 최초 생성 시간·추가 IB 메모리가 필요하다. meshlet·mesh shader·GPU 선택을 구현한 것으로 설명하지 않는다.

### Emerald Square Day 후보: 규모와 가시성

출처는 [NVIDIA ORCA Emerald Square](https://developer.nvidia.com/orca/nvidia-emerald-square)다. `/home/suhan/dev/assets/EmeraldSquare_v4_1.zip`은 617,345,248B이며 `/home/suhan/dev/assets/EmeraldSquare_v4_1`에 362개 항목·1,544,054,516B를 추출했다. 원본 `EmeraldSquare_Day.fbx`·`EmeraldSquare_Day.pyscene`·`symmetrical_garden_4k.hdr`를 그대로 사용하고 포맷을 변환하지 않는다. 배포 README의 unique triangles 2,695,054·total triangles 10,046,405는 원본 수치이며 런타임 제출량이 아니다.

CMake `DY_SHOWCASE_EMERALD_SCENE`·`DY_SHOWCASE_EMERALD_ENVIRONMENT`가 원본 FBX와 HDR를 선택하고 `build/showcase emerald`로 연결한다. 기본 A/B는 `INDEXED LOD`다. 원본 약 64.17초 Camera 클립 전체를 60초로 재생하며 끝 구도를 유지한다. 장면 경계와 HDRI를 드러내던 추가 15초 조감 동선은 제거했다. Space로 정지하고 L로 LOD ON/OFF, C로 단계색, F9로 지원 기기의 실제 삼각형 wireframe, F11로 제출량을 볼 수 있다. F5/F6 정상 렌더 비용 비교에서는 C와 F9를 끈다. 카메라 scale·PostRotation을 포함한 노드 체인을 사용하며 100배 큰 legacy Camera.Position은 사용하지 않는다. +Y up·+Z front·+X coordinate, UnitScaleFactor=100(미터)이며 ufbx의 원본 축을 유지한 뒤 예제에서 Y-up→Z-up 회전을 한 번 적용한다. 렌즈는 수직 FOV 50도 근사이며 정확한 원본 재현이나 구도·관통 없는 경로를 확인했다고 주장하지 않는다.

ORM은 R=AO·G=roughness·B=metallic, normal은 DirectX 방식이며 BaseColor alpha를 opacity에 연결한다. Bistro의 비어 있는 AO 채널 보정은 Emerald에 적용하지 않는다. FBX의 광원과 HDRI intensity 1을 유지하며 고정 노출 대신 GPU 로그 평균 휘도 측광·시간 적응을 연결한다. Emerald는 `advanced.autoExposure=true`, `exposureKey=0.18`, `exposureAdaptationSpeed=1.5`이고 공통 기본 자동 노출은 OFF다. -/+는 보정값을 바꾼다. 원본 Falcor의 자동 노출 누락을 보완하는 구성이지 다른 톤매퍼의 동일 출력 재현은 아니다. 카메라·광원 포함은 배포 CHANGELOG v4의 구성이다. 원본 `.pyscene`은 실행하지 않는다.

라이선스는 CC BY-NC-SA 3.0이며 Nicholas Hull·Kate Anderson·Nir Benty, NVIDIA ORCA, 식생의 SpeedTree 크레딧과 수정 내역을 영상에 남긴다. 실제 재질·조명·가시성·LOD 감소·메모리·FPS는 아직 측정하지 않았다. LOD 대표 장면 채택은 근중원경의 실제 단계 선택·제출량 감소와 목표 모바일 기기의 메모리·프레임 비용을 기준으로 결정한다. 전체 삼각형 수와 데스크톱 로딩만으로 적합성을 확정하지 않는다. 같은 시각·구도로 F5/F6을 수집하고 디테일 근사와 중복 작업 제거를 구분한다.

### 보관한 후보: Jungle Ruins

출처는 [Intel 공식 장면 배포](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)다. /home/suhan/dev/assets/JungleRuins_1_0_1b.zip은 4,254,165,506B(약 4.0GiB)로 다운로드했고, 355개 항목·약 7.44GB 원본 USD 묶음을 `/home/suhan/dev/assets/JungleRuins_1_0_1b`에 추출했다. 추출 완료는 엔진 로딩 완료가 아니다.

원본 USD/Blender는 현재 모델 로더의 직접 지원 형식이 아니며 포맷 변환 경로도 채택하지 않는다. 광역 시연은 원본 FBX 에셋 Emerald로 선정했다. Jungle 원본은 보관하며 이번 시연에서 원본 로더 지원·엔진 통합·촬영을 추진하지 않는다.

원본 USD가 참조한 [Poly Haven Kloppenheim 06 Pure Sky](https://polyhaven.com/a/kloppenheim_06_puresky)의 2K HDR를 `/home/suhan/dev/assets/JungleRuins_1_0_1b/kloppenheim_06_puresky_2k.hdr`에 별도로 내려받았다. 원본 USD의 8K 경로는 그대로다. 제작자는 Greg Zaal·Jarod Guest, 라이선스는 CC0다. 이 준비 이력은 geometry·재질 로딩이나 시연 통합 성공을 의미하지 않는다.

Jungle 준비 이력을 Emerald의 로딩·실행 결과와 섞지 않는다. 제작자가 설명한 instance 확장 삼각형 수를 실시간 제출량으로 쓰지 않는다.

### Dense Arena: 반복과 제출 비용

[Dense Arena](arena/README.md)는 Enter the Gungeon처럼 탑다운에서 적·탄환·파편·소품이 많은 장면을 목표로 하며 해당 게임의 에셋을 복제하지 않는다. 현재 소스는 [Kenney Tiny Dungeon](https://kenney.nl/assets/tiny-dungeon)의 CC0 1.0 방·가구·적 이미지와 자체 탄환·파편을 사용한다. 에셋은 /home/suhan/dev/assets/kenney_tiny-dungeon을 참조하며 저장소로 복사하지 않는다.

연결된 9개 방에서 기본 적 600·탄환 4,500·파편 900개의 고정 풀과 별도 바닥·벽·소품·플레이어를 구성한다. 결정론적 시각에 따라 이동·수명이 순환하며 geometry·재질·이미지는 시작 시 공유하고 변환을 갱신한다. 새로운 GPU 자원을 매 프레임 생성하는 구조가 아니며 충돌·실제 플레이 구현은 아니다.

공통 dyf Scene/Renderer의 alpha-mask quad 경로이며 조명·HDR을 끈다. 같은 mesh/material의 인접 엔티티를 instancing한다. 서로 다른 모든 물체를 한 draw로 그린다고 설명하지 않는다. 기본 linear sampler이므로 엄밀한 nearest-neighbor 픽셀 필터도 아니다. GPU culling·indirect·LOD를 이미 연결한 장면으로 표기하지 않는다.

## advanced 01–23 통합 목표

| 예제 묶음 | 장면에서 증명할 내용과 남은 범위 |
| --- | --- |
| 01 Profiling / 19 MultithreadRecording / 21 RenderGraph | CPU 작업·대기와 GPU span/패스를 구분하고 실제 worker 기록·활성 패스를 보여 준다. A/B는 실측 결과를 수집한 뒤 사용한다. |
| 02 DepthReverseZ / 03 DepthSampling | 큰 깊이 범위와 Surface 기반 효과가 같은 투영·clear·비교·깊이 복원 계약을 사용한다. |
| 04 StencilMask | 실제 depth-stencil 자원과 선택 영역/마스크를 연결한다. 단순 색상 마스크와 구분한다. |
| 05 ResourceBarriers / 06 FramesAndPresent | 그래프 의존성·상태 전환·완료 fence·present 수명을 유지한다. 창/앱 수명주기와 플랫폼 연결을 포함한다. |
| 07 PBRLighting / 08 ShadowMapping / 09 ClusteredLighting | Bistro의 재질과 directional/point/spot·그림자를 사용한다. 4개 점광원 지원과 clustered GPU light-list 소비 완료는 별개다. |
| 10 DeferredLighting / 11 HDRPostProcess / 12 TemporalAA / 13 VolumetricFog | 실제 Surface·HDR·TAA 연결을 기반으로 완전한 deferred와 깊이 기반 fog를 완성한다. Surface debug만으로 deferred 완료라 하지 않는다. |
| 14 InstancingBatching / 15 ResourceReuse | Dense Arena의 반복 geometry·동적 transform·고정 풀로 배치와 재사용을 보여 준다. |
| 16 BindlessMaterials / 17 IndirectDrawing / 18 GPUCulling | 플랫폼 한도에 맞는 재질 접근과 GPU 가시 목록→indirect 데이터 흐름을 장면에 연결한다. 독립 indirect 구현과 환경/게임 통합을 구분한다. |
| 20 AsyncCompute | 실제 큐 선택·동기화·필요한 ownership 이전과 중첩 실행을 구현한다. CPU 병렬 기록과 구분한다. |
| 22 MeshShaderBasics / 23 MeshletCullingLOD | 공통 renderer에 CPU 선택 indexed LOD를 연결했다. 독립 23의 meshlet 생성·GPU 선택·실행은 여전히 미완성이며 mesh shader도 별도 선택 경로다. |

독립 예제는 API 설명용으로 유지한다. 영상은 위 기능들을 장면별 질문으로 묶고, 구현 목표의 전체 목록과 시연할 대표 기능을 구분한다. RT 24–28은 구현·영상 범위 밖이다.

## 남은 구현과 촬영 준비

1. 현재 A/B·profiler·Dense Arena 소스를 기존 빌드 대상에 연결한다. 실행 결과와 CPU/GPU 개선폭은 실제 측정 전까지 미측정으로 남긴다.
2. 이번 Bistro local probe·유리 직접광 차폐·외부 식생 움직임의 화면과 초기 준비/정상 프레임 비용을 확인한다. 이후 공간별 lightmap/probe volume·GI 시간 안정화·다층 굴절을 구분해 구현하며 비용과 화질을 함께 관리한다.
3. 원본 FBX Emerald Day의 60초 카메라·자동 노출·ORM·shared geometry를 확인하고 같은 시각의 indexed LOD A/B를 수집한다. 단계색·wireframe으로 기하 변화, 정상 렌더와 profiler로 화질·비용을 확인한다. 포맷 변환은 진행하지 않는다. 실제 결과를 확보한 후 GPU culling·indirect 통합 범위를 넓힌다.
4. Dense Arena는 현재 CPU 컬링·instancing의 증거를 먼저 확보하고 이후 GPU culling·indirect를 연결한다. 독립 기능 존재만으로 장면 통합 완료를 선언하지 않는다.
5. Windows_DX/Windows_VK를 빌드·실행하고 Android/iOS는 별도 브랜치의 native surface·입력·앱 수명·에셋 접근·shader packaging을 연결한다. 같은 세계·동선·게임 상태를 유지하며 기기별 해상도·텍스처·그림자·후처리 품질은 명시한다.
6. 실제 캡처와 A/B 결과가 있는 항목만 편집에 넣는다. 미완성 모바일 구간을 다른 플랫폼 영상으로 대체해 완료처럼 표기하지 않는다. 에셋 출처·라이선스·수정/변환 내역을 크레딧에 남긴다.

사용자 확인 항목은 Bistro 카메라·재질·실내 probe 반사와 박스 경계·유리 직접광 차폐·잔여 흰 점·외부 식생과 그림자/motion·입력/resize, Emerald 60초 원본 카메라의 끝 구도·자동 노출·재질/조명, 정지/이동 TAA 흔들림·잔상, 공유 geometry 배치·LOD 전환/alpha/wireframe/통계, 초기 probe·LOD 생성 비용과 메모리, A/B 바람 상태까지 동일 복귀·조건 변경 취소·캐시 무효화, Dense Arena instancing 전후 draw/instance/triangle, 플랫폼별 실제 실행이다. 실행·캡처·성능 측정은 수행하지 않았으며 빌드는 실행 파일 제공 작업에 포함한다.
