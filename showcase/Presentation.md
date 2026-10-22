# 렌더링 기술 범위·구현 현황·발표 대상

전체 100%는 아래의 렌더링 기술 영역 전체를 뜻한다.
분류는 [Real-Time Rendering의 목차](https://www.realtimerendering.com/)를 참고해
실행 구조·영상 재구성까지 묶었다. RT와 특수 렌더링도 전체 지도에 포함한다.
기술마다 규모와 대체 관계가 달라 전체 완성률을 단일 %로 합산하지 않는다.
**● 구현 경로 있음 / △ 제한 구현·독립 예제만 / — 미구현**으로 표시한다.
구현 표시는 소스 기준이며 화질·성능·플랫폼 검증 완료와 구분한다.

## 전체 기술 영역에서 현재 구현한 것

1. **장면·기하·애니메이션**
   ● glTF/FBX, 메시·변환·카메라, 골격·모프, 공유 geometry.
   △ Tessellation은 RHI 경로. — 전용 지형·세분 곡면·변위 시스템,
   GPU 파티클·모발·천 등 특수 기하 렌더링.

2. **가시성·디테일·GPU 구동**
   ● CPU/GPU frustum, Hi-Z 가림, indexed LOD, instancing,
   가시 목록 압축 → indirect draw. △ 정적 메시 중심 LOD.
   — 변형 메시 LOD, meshlet 컬링·LOD, 가상 기하·대규모 공간 스트리밍.

3. **표면 재질·텍스처**
   ● Metallic/Roughness PBR·GGX·Fresnel, normal/AO/발광,
   mip·이방성 필터, Mask/Blend·양면·unlit.
   — Clearcoat·재질 이방성·Sheen·SSS·복합 층 재질.

4. **직접광·그림자**
   ● 방향·점·스폿, 사각·원형 LTC 면적광, Clustered Lighting,
   CSM·atlas·PCF·가변 반그림자 필터·정적 그림자 캐시.
   — 면적광 그림자, 투과·유색 그림자.

5. **환경광·간접광·GI**
   ● HDRI·SH·GGX IBL, 다중 정적 프로브·box projection·가림,
   SSAO·SSGI와 시간·공간 필터. △ 정적·화면 공간 기반 GI.
   — Lightmap 베이커, DDGI·동적 radiance cache·일반 다중 반사 GI.

6. **반사·투명·굴절**
   ● SSR·IBL 보완, 알파 합성. △ Fresnel·IOR·굴절·흡수의
   1/2층 래스터 유리. — 평면 반사 전용 경로, 임의의 중첩 매질,
   다층 재귀 굴절·분산·caustics.

7. **매질·하늘·자연 현상**
   ● 높이 밀도·광원·그림자를 사용하는 단일 산란 볼륨 안개.
   — 다중 산란 대기·구름, 전용 물·해양·유체 렌더링.
   식생 움직임은 노드 변환이며 전용 잎 변형·산란 모델은 미구현.

8. **샘플링·AA·영상 재구성**
   ● TAA·motion·history 제어, HDR supersampling, 효과별 노이즈 필터.
   — MSAA·FXAA·SMAA의 공통 렌더 경로,
   DLSS/FSR/XeSS 등 업스케일러·프레임 생성·동적 해상도 제어.

9. **색·카메라·후처리·출력**
   ● 선형/sRGB, HDR 내부 렌더, 노출·자동 노출·bloom·톤 매핑,
   색보정·선명화·고정 해상도·비율 유지.
   — 3D LUT, DOF·motion blur·렌즈 효과, HDR 디스플레이 출력.

10. **렌더 구조·자원·최적화**
    ● Forward·Clustered, 모듈별 패스, RenderGraph·배리어·fence,
    병렬 기록·재사용·timestamp·A/B. △ Deferred·Mesh Shader는 독립 예제,
    Bindless는 고정 배열. — Async Compute, 완전 Bindless,
    Virtual Texture·sparse residency·수요 기반 스트리밍·VRS.

11. **광선 기반 렌더링**
    — BLAS/TLAS·Ray Query·RT Pipeline, RT 그림자·반사·GI,
    Path Tracing·다중 경로 적분·광자 기반 기법.
    RT는 이번 개발·발표 범위에서 제외했으나 전체 기술 영역에는 포함한다.

12. **특수 표현·시점**
    — NPR·전용 윤곽선/스타일화, SDF·복셀·점/스플랫 표현,
    XR stereo·multiview·foveated rendering, 스펙트럼·편광 렌더링.

## 이번 브랜치에서 개발한 범위

아래는 과거 기능 개발 범위를 정리한 기록이다.
당시 비교 기준은 `main:89ac61c` → `modular-rendering-showcase`이며,
현재 `performance` 브랜치의 커밋 비교 기준과 구분한다.
기존에 PBR·normal/AO·직접광·그림자·골격/모프·CPU 컬링·인스턴싱,
HDR·노출·톤 매핑·RenderGraph·병렬 기록이 있었다.

- **신규 공통 기능:** HDRI/SH/GGX·프로브·가림, SSAO/SSGI/SSR·필터,
  래스터 유리·흡수, LTC, indexed LOD, GPU frustum/Hi-Z·indirect,
  자동 노출·색보정·선명화.
- **독립 예제 → 공통 렌더러 통합:** Clustered·TAA·볼륨 안개,
  표면 정보 출력·bloom·후처리. 공통 렌더러는 Forward이며
  Deferred와 Mesh Shader는 독립 예제 범위다.
- **기존 기능 개선:** geometry 공유·원본 재질 해석,
  광원 크기 기반 그림자 설정·캐시·caster/PSO·자원 재사용,
  1080p 출력·상세 계측·A/B.
- **구조·API:** RenderExtension과 선택 모듈 6개
  (Environment·Clustered·Surface·Fog·Transparency·PostProcess),
  RHI DrawIndirect·DrawIndexedIndirect·DispatchIndirect 3종과 backend 연결.
- **예제·시연:** 독립 17·18 구현, Bistro·Emerald·Arena,
  구형 Sponza 비교 실행 파일과 공통 카메라, `showcase/` 분리.

advanced 01–23은 실행 경로 **20개(약 87%)**, 제한 구현 **1개(16)**,
미구현 **2개(20 AsyncCompute·23 MeshletCullingLOD)**다.
이 수치는 예제 목록의 비율이며 전체 렌더링 기술의 완성률은 아니다.
이번 브랜치는 기존 래스터 기반에 고급 효과·GPU 구동·진단 구조를 확장했다.

이번 성능 측정의 코드 기준은 `performance`의 **정리한 기본 커밋 9개 + 성능 커밋 4개**다.
이후 발표 문서 정리는 그 코드 변경과 구분한다.
위 기능 목록은 과거 개발 범위의 설명이며, 성능 커밋은 그 렌더 경로의 중복 계산·기록·자원 처리를 줄인 작업이다.
발표에서도 기능 추가와 같은 화질 조건의 성능 개선을 따로 설명한다.

## 발표 흐름 초안 — 10분 가정

발표의 질문은 “같은 렌더러가 재질·조명, 큰 장면, 반복되는 애니메이션을 어떻게 처리하는가”로 잡는다.
**Bistro → Wine 또는 Night → Emerald → Arena → 구조** 순서로 장면마다 역할을 맡긴다.
기술 목록 전체를 읽기보다 결과 화면, 효과를 설명하는 정지 화면, 관련 패스·수치를 연결한다.

발표 길이는 아직 확정하지 않았으며, 10분은 시간 배분의 예시다.
아래 shot과 장면 시각은 초안 후보다. Emerald 15초 부근, Sponza 시작 구도,
Arena의 카메라 투어는 이번 탐색에서 화면을 살펴봤으며, 나머지 시각은 시연 후보로 남긴다.
Cinematic은 `--start N --paused` 또는 좌우 5초 이동·Space 정지를 사용한다.
발표 시간과 장면의 재생 시각은 별개다.

| 발표 시간 | 장면·shot | 장면 시각·상태 후보 | 토글·표시 | 청중에게 할 말 |
| --- | --- | --- | --- | --- |
| 0:00–0:40 | Bistro exterior 대표 전경 | 시작 0초에서 재생, 외부 공간과 재질을 함께 보여준다 | 안내·Profiler를 닫은 결과 화면 | “외부 모델을 불러와 공통 재질·조명·후처리 경로로 그린 화면입니다.” |
| 0:40–2:15 | Bistro 재질·환경광 정지 화면 | 후보 15초, 재질과 주변 반사가 보이는 구도로 정지 | `V`로 base color·normal·roughness를 짧게 확인한 뒤 원복. `F2/F3` 환경 확산·반사 또는 `7/9` AO·SSR 중 한 비교 선택 | “표면 정보가 조명 입력이 되고, 직접광과 환경광·화면 공간 효과가 최종 화면에 합쳐집니다.” |
| 2:15–3:45 | **선택 A: Wine interior**의 유리·병·잔 | 후보 0–10초 중 유리가 겹쳐 보이는 구도에서 정지 | `F10` 1/2층 유리. 필요하면 `I` 프로브 비교. `interior`는 Wine FBX를 선택한다 | “Fresnel·IOR·흡수와 두께를 사용하는 래스터 유리입니다. 실내의 기본 간접광은 방 프로브이며 SSGI는 기본 OFF입니다.” |
| 2:15–3:45 | **선택 B: Bistro night**의 전등·간판·바닥 | 후보 15–25초 중 발광체와 비춰진 표면이 함께 보이는 구도에서 정지 | `F1` 직접광 전체 또는 `P` 부드러운 그림자 비교 중 하나. `J`는 clustered 경로 설명에 사용 | “엔진 시연용 야간 조명입니다. 점·스폿·LTC 면광원을 배치하고, 많은 광원을 공간별 목록으로 처리합니다.” |
| 3:45–5:45 | Emerald의 넓은 거리·건물·식생 | 15–35초 구간을 보다가 같은 시각에 정지 | `L` LOD, `C` 단계색, `F9` wireframe을 설명 후 원복. `U/O` GPU frustum·Hi-Z 또는 LOD A/B 중 하나 선택 | “화면에서 필요한 상세도와 가시 목록을 만들어 큰 장면의 제출량을 줄입니다.” |
| 5:45–7:45 | Arena의 기사·해골·투사체와 반복 소품 | 기본 72명으로 재생 후 Space 정지 | `F5/F6` 동일 상태의 instancing A/B. draw 수와 CPU RECORD/SUBMIT 표시 | “반복되는 geometry·자원을 공유하고, 골격 애니메이션과 다수 객체를 같은 렌더 구조로 처리합니다.” |
| 7:45–9:15 | 방금 본 장면을 구조·패스와 연결 | 대표 장면 하나의 정지 화면과 Profiler | dyf → 선택 모듈 → RenderGraph → RHI. CPU 작업·대기와 GPU 패스 시간을 구분 | “같은 장면 API 아래에서 모듈이 패스와 자원 의존성을 연결하고 각 backend가 명령을 기록합니다.” |
| 9:15–10:00 | 구현 범위·결과·남은 일 | 측정 조건 표와 지원 범위 표시 | Linux/Vulkan 실행 결과와 다른 플랫폼의 구현·실기 확인 범위를 구분 | “실행 결과로 확인한 범위와 구현만 연결한 범위를 구분하며, RT 등 이번 범위 밖의 기술도 남겨 두었습니다.” |

Wine과 Night는 같은 시간 슬롯의 대안이므로 본편에서는 하나를 선택한다.
유리·흡수를 강조하면 Wine, 다수 광원과 야간 조명을 강조하면 Night를 사용한다.
Night의 `F1`은 LTC만 분리하는 토글이 아니므로 직접광 전체의 비교로 설명한다.
면광원 그림자는 현재 지원 범위에 포함하지 않는다.

Emerald는 밝은 잎과 어두운 그늘의 대비가 강하다.
미관을 대표하는 장면보다 **규모·LOD·컬링 시연**에 배치하고,
잎과 그늘의 정보를 함께 보여줄 수 있도록 카메라·노출·톤 매핑의 표시 마감을 조정할 후보로 남긴다.
간단한 원리 설명이 필요하면 외부 에셋 없는 `culling`의 공유 메시 장면을 예비로 사용한다.
GPU 가시 개수 readback은 미구현이므로 가시 객체·삼각형의 정확한 개수 대신
실제 GPU 패스 시간과 indirect batch, CPU 경로에서 제공하는 통계를 구분해 제시한다.

## 현재 측정 기록과 발표에서의 사용

아래 Bistro 수치는 RTX 3060·Vulkan의 **headless 환경**에서 측정한 완료 제출/s다.
내부 렌더 1920×1080·출력 636×1024, TAA 기본 OFF, warm-up 120회 뒤 측정 3,600회,
프레임당 장면 시각 1/60초의 정해진 카메라 경로를 사용했다.
각 프리셋의 기본 조명·품질을 유지했으며, 서로 다른 장면의 FPS를 같은 작업량의 대결로 해석하지 않는다.
특히 Night와 Exterior는 광원 구성이 다르다.

| 장면 | 현재 기록 | 조건·해석 |
| --- | --- | --- |
| Bistro Night | 61.86 FPS | headless, 내부 1080p, warm-up 120 + 측정 3,600회 |
| Bistro Exterior | 77.25 FPS | 같은 측정 절차와 해당 장면의 고정 카메라 경로 |
| Bistro Interior / Wine | 127.40 FPS | 같은 측정 절차와 실내의 고정 카메라 경로 |
| Emerald Day | 52.98 FPS | 장면 15–35초, warm-up 120 + 측정 1,200회, 내부·출력 1920×1080, VSync/HUD OFF |
| Sponza | 87.60 FPS | 장면 0–20초, warm-up 120 + 측정 1,200회, 내부·출력 1920×1080, VSync/HUD OFF |
| Arena | 캡처 당시 HUD 28 FPS | 기본 해골 72명 + 기사 1명. VSync·Profiler ON의 별도 시연이므로 위 평균 FPS와 비교하지 않는다. 당시 CPU 5.83ms / GPU 12.62ms 표시 |

Emerald와 Sponza는 캡처 1회를 포함한 짧은 탐색이다. 서로 다른 카메라 구간과
기본 조명·노출 설정을 사용했으므로 각 구도에서 비싼 패스를 찾는 자료로 사용한다.
Bistro의 전체 경로 기록과 직접 성능 회귀를 판정하는 자료로 사용하지 않는다.

headless 완료 제출/s는 **실제 Sway 화면에 표시된 FPS와 구분**한다.
Sway의 출력 창 크기·배치·swapchain 획득 대기가 달라지면 내부 1080p가 같아도 처리량이 달라질 수 있다.
발표용 수치는 내부 해상도와 실제 출력 크기, 장치·driver, VSync, 카메라·시각을 함께 기록한 뒤 확정한다.
60 FPS는 위 Night 측정 조건에서 얻은 결과이며, 모든 장면·플랫폼의 실시간 표시 성능을 뜻하지 않는다.
Windows D3D12/Vulkan과 Android/iOS의 실기 결과는 현재 자료와 별도로 확보된 범위만 제시한다.

## 이번 탐색에서 고른 비교 사례

**Emerald의 식생 근접 구간**을 후속 비교 대상으로 고른다. CPU 평균은 5.60ms,
업로드→그래프 구간은 0.062ms이며, GPU 그래프 평균은 18.81ms다.
투명 nearest/rear/front 합계가 7.48ms로 가장 크고 Shadow 3.81ms, Main 1.89ms가 뒤따른다.

| Emerald 장면 시각 | 프레임 간격 평균 | 투명 3패스의 구간 평균 근사 |
| --- | --- | --- |
| 15–20초 | 26.81ms | 14.91ms |
| 20–25초 | 22.74ms | 11.36ms |
| 25–30초 | 14.13ms | 2.46ms |
| 30–35초 | 11.76ms | 1.28ms |

GPU 값은 지연 게시된 profiler 결과를 관측 구간별로 묶은 근사다.
Sponza의 이번 구간에서는 GPU 그래프 11.35ms 중 투명 합계가 0.45ms,
Shadow가 3.60ms였다. 이번 두 표본의 비용 차이는 주로 투명 처리에서 나타났다.

코드상 FBX BaseColor의 기본 레벨에 중간 alpha가 한 픽셀이라도 있으면 Blend로 분류되며,
Blend는 transmission이 0이어도 투명 경로를 사용한다. alpha가 0인 영역도 최종 coverage를
곱하기 전 조명 계산을 거치는 구조다. 다만 **실제 Emerald의 어떤 식생 재질이 Blend인지,
해당 분류가 원본 의도와 다른지는 아직 확정하지 않았다.** 흰 잎·강한 대비의 원인도 별도로 남긴다.
다음 비교는 이 구간의 재질 분류와 화면 점유를 확인하는 것이며, 지금 임의의 Mask 전환이나
식생 전용 최적화를 적용하지 않는다.

Arena는 애니메이션·반복 객체를 설명하는 본편 후보로 유지한다.
실시간 카메라 투어 캡처의 낮은 FPS는 VSync와 Profiler가 켜진 조건까지 포함하므로,
GPU 작업량만으로 설명하거나 Cinematic 결과와 순위를 매기지 않는다.

## 예비 시연과 연결 대기

- **Sponza:** Bistro와 역할이 겹쳐 본편에서 제외한다. 공통 에셋·카메라로 초기/현재 결과를 설명하는 예비 시연이며,
  렌더 경로·조명 조건이 다른 실행 파일 사이의 직접 FPS 대결에는 사용하지 않는다.
- **Intel Knight:** Cinematic의 `--actor knight`로 추가할 수 있다. Arena의 골격 애니메이션과 역할이 겹치므로 선택 사항이다.
- **Culling:** 단순한 공유 메시 장면에서 GPU/CPU·LOD·Hi-Z·indirect의 관계를 설명하는 예비 시연이다.
- **Jungle Ruins USD:** 원본은 있으나 입력 변환·장면 연결 대기다. 본편의 실행 가능 장면으로 제시하지 않는다.
- **Dusk 프리셋:** 별도 야간·황혼 원본의 장면·환경·카메라 연결 대기다. 현재 `emerald`는 Day 프리셋이다.

다음 에셋 선택은 본편의 시연 역할과 화면을 기준으로 한다.
여러 로컬 에셋을 살펴본 뒤 유독 느린 장면만 같은 내부·출력 조건에서 비교하고,
그 원인이 장면의 geometry·재질·조명 구성인지 별도 연결 문제인지 정리한다.
