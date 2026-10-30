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

비교 기준은 `main:89ac61c` → `modular-rendering-showcase`다.
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

## 발표에 넣을 기술

- **빛·재질 — Bistro:** PBR·normal·Fresnel, 직접광·LTC·그림자,
  HDRI·다중 프로브·SSAO/SSGI·SSR, 유리·굴절·흡수,
  안개·노출·톤 매핑·선명화. 표면 정보와 활성 패스로 결과를 연결한다.
  식생·Knight 애니메이션을 포함하고 Sponza는 초기/현재 비교에 사용한다.
- **규모 — Emerald·Culling:** frustum·Hi-Z, 화면 오차 기반 LOD,
  가시 목록 압축·indirect. 단계색·wireframe·제출 삼각형과 GPU 패스 시간.
- **반복 — Dense Arena:** 많은 적·탄환·소품, geometry·자원 공유,
  instancing·컬링·병렬 기록. 동일 상태에서 draw 수·CPU RECORD/SUBMIT 비교.
- **구조·멀티플랫폼:** 기본 dyf + 선택 모듈 + RenderGraph + RHI,
  D3D12/Vulkan/Metal의 공통 계약, 장치 기능·한도와 품질 예산.
  Windows DX/VK와 별도 브랜치의 Android/iOS는 실기 결과를 확보해 제시한다.
- **Profiling·최적화:** CPU WORK·대기, GPU SPAN·패스별 ms,
  draw·instance·triangle. 그림자 캐시·병렬 기록·LOD·instancing을
  동일 카메라·시각·해상도에서 한 항목씩 A/B로 비교한다.

Linux/Vulkan 빌드 완료. 이번 변경의 화질·안정성·60 FPS와
Windows·모바일 실기 성능은 미확인. GPU 가시 개수 readback은 미구현이다.
촬영 기준은 1920×1080·16:9·TAA 기본 OFF이며 기기별 차이를 명시한다.
