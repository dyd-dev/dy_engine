# 렌더링 정확성·일반성 감사

작성일: 2026-10-23. 실행 증거와 소스 검토, 미확인 범위를 구분한다.

엔진·importer·회귀 검사 기준 커밋은 `9ecaa8b`다. 실행 인자·품질·크기·구간은 각 CSV와 시작 로그에 남기며 [실행 파일·증거 해시](../build/showcase/conformance-provenance.json)를 보존했다. Cinematic 측정은 이 감사의 원본 해석 수정본이며, 이후 추가한 skin transform cache는 actor 없는 다섯 장면에서 사용되지 않는다. 해시 기록도 당시 측정 파일과 최종 실행 파일을 구분한다.

확인한 핵심 결함은 원본 정점 색상 누락, coverage와 optical transmission의 혼합, 알파를 무시한 투명 깊이 분할, 투명 IBL의 BRDF 적분 누락, 일부 Surface 법선 변환의 불일치다. 해당 경로를 수정했다. NativeMoments는 보간 정밀도 반례를 확인하여 FP32 수동 보간으로 복원했다. 이 결론은 모든 장면·장치의 픽셀 동등성이나 전체 glTF/FBX 지원을 보장하지 않는다.

Emerald의 원본 식생 texture는 녹색이다. 기존의 흰 식생·검은 그늘을 원본의 특징으로 설명한 것은 잘못이었다. 같은 카메라·노출·조명에서 ORCA 재질 해석을 수정한 화면을 비교했고, 잎의 색과 그늘의 정보가 회복됨을 확인했다. 개별 수정의 기여도를 모두 분리한 실험이나 원저자의 전체 조명 재현을 뜻하지 않는다.

## 검증 상태

| 구분 | 증거 | 한계 |
|---|---|---|
| 실제 실행 환경 | Debian GNU/Linux 13, NVIDIA GeForce RTX 3060, Vulkan | 다른 GPU·드라이버·운영체제의 실행 증거 없음 |
| CPU 모델 테스트 | 실제 `LoadModel` 회귀 검사 실행. 정점 RGBA, alpha/T, flat normal·skin·morph, source axes·camera·CullingOff, strict 거부와 명시 fallback 검사 | 지원 형식의 모든 조합을 열거한 인증은 아님 |
| GPU 수치 fixture | 기본·advanced·투과·법선·projective visibility·실제 glTF·Vertex/Compute skinning·투명 1/2층·alpha-aware nearest·행렬 캐시 OFF/ON의 35-frame 실행 통과. Vulkan error 0 | 사용하지 않는 Model shadow vertex attribute 경고 2건. 해당 GPU·조건의 fixture 통과이며 모든 입력의 보증은 아님 |
| HLSL 컴파일 | DXC 1.9.0.5512로 stock·AdvancedRendering·Model의 CMake 변형 49개를 DXIL로 컴파일, 최종 49/49 성공·진단 0 | D3D12 GPU 실행·Metal 실제 컴파일 미실행 |
| 알파 수식 | Opaque/Mask/Blend와 transmission 조합 36개 스칼라 사례에서 기대 coverage와 일치 | 실제 셰이더·블렌딩·깊이 출력 검사는 GPU fixture 결과로 별도 기록 |
| LTC 다항식 | `r∈[0, 1/4]` 100,001개 FP32 비 FMA 스칼라 표본의 최대 상대오차 약 `3.76e-8` | 실제 셰이더의 FMA·컴파일러·GPU별 결과는 별도 검증 |
| NativeMoments | 제한된 보간 정밀도 모델에서 수치 반례 확인, native 분기 제거 | 반례는 실제 RTX 3060 이미지 오차 측정값이 아님 |
| 원본 비교 | Blender 4.5.14 CPU 렌더와 glTF 정점 색상 14,196개 내부 픽셀 비교: 최대 RGB 차이 1 byte. Emerald bounds·재질 수와 duplicate 수 조사 | 색상 비교는 unlit fixture. Emerald 전체 PBR 조명·모든 DCC 동등성 인증은 아님 |
| 모바일 | 소스의 capability gate와 메모리 payload 계산 | Android/iOS 실기 실행·메모리·FPS 미측정 |

테스트 소스: [ModelConformanceTests.cpp](../src/Extends/Model/tests/ModelConformanceTests.cpp), [RenderingConformanceTests.cpp](../src/Extends/Model/tests/RenderingConformanceTests.cpp). 테스트 코드가 존재하거나 셰이더가 컴파일된 사실을 실행 통과로 간주하지 않는다.

최종 [GPU 실행 로그](../build/showcase/rendering-conformance.log)는 256×256, analytic RGB 허용 차이 1–2 byte다. 투명 1층 5프레임·2층 nearest 7프레임을 실제 실행했고, alpha 0의 유령 면 뒤 두 실제 투명층과 IOR 1 굴절의 색상을 검사했다. Vertex/Compute 스키닝은 각각 12,676/12,007개 색상 내부 픽셀을 비교했다. 고급 HDR 행렬 캐시 OFF/ON은 Blend와 실제 lit shadow receiver를 둔 Mask 각각 **전체 RGBA 262,144 byte의 차이 0**, bounds·geometryRevision 동일, deformation history 유지를 확인했다. [CTest 로그](../build/showcase/cpu-conformance.log)는 CPU 두 target 모두 통과다.

HLSL 최초 컴파일은 `point`가 예약어인 두 곳에서 실패했다. [투명 LTC](../src/Extends/AdvancedRendering/Shaders/transparent_ps.hlsl)와 [cluster cell](../src/Extends/AdvancedRendering/Shaders/clustered_lighting_cs.hlsl)의 변수 이름만 수정한 뒤 전체를 재컴파일했다. [실행 기록](../build/showcase/hlsl-conformance.log)을 남기고 임시 DXIL은 제거한다. reflection의 register는 모두 `space0`이며 [D3D12 root signature](../src/Backends/D3D12/D3D12Device.cpp)의 공간과 일치한다. 이 사실은 실제 pipeline 생성·픽셀 출력 성공과 구분한다.

## 범위별 근거와 한계

| 범위 | 확인·수정한 계약과 관련 소스 | 현재 한계·검증 항목 |
|---|---|---|
| Importer geometry | [glTF](../src/Extends/Model/GltfModelLoader.cpp), [FBX](../src/Extends/Model/UfbxModelLoader.cpp)의 위치·인덱스·인스턴스·정점 RGBA 보존. NORMAL이 없는 glTF는 corner를 분리하여 flat frame 생성 | 모든 importer의 모든 primitive·확장 지원을 의미하지 않음. Emerald의 원본 duplicate도 그대로 유지 |
| 좌표·단위 | [Model API](../src/Public/dyf/Extends/Model/Model.h)에 `sourceToYUp`, `sourceUnitMeters` 메타데이터 보존 | source 변환과 showcase의 Y-up→Z-up·크기 정규화를 구분. 특이 행렬의 유일한 물리적 법선은 정의할 수 없음 |
| Materials | 원본 factor·texture channel·선형 정점 색상 전달. FBX importer의 명시적 채널 해석 옵션과 node별 CullingOff 보존 | FBX 제작 도구별 의미가 동일하지 않음. CullingOnCW의 전면 방향 선택은 미지원. ORCA 규약을 모든 FBX에 강제하지 않음 |
| Alpha coverage | glTF 원본 alphaMode 보존. `Opaque=1`, `Mask=test 후 1`, `Blend=clamp(alpha)` | 중간 alpha가 있다는 이유만으로 모든 재질을 Mask 또는 Blend로 강제하지 않음 |
| Optical transmission | T가 양수라는 이유로 glTF alphaMode·doubleSided를 덮어쓰던 처리 제거. 라우팅은 `Blend || transmissionFactor>0` | 미지원 transmission/thickness texture는 기본 거부. 명시 fallback 허용 때만 근사. 완전한 volume path tracing이 아님 |
| 투명 깊이·조명 | [TransparentRendering](../src/Extends/AdvancedRendering/TransparentRendering.cpp), [PS](../src/Extends/AdvancedRendering/Shaders/transparent_ps.glsl): nearest도 texture·정점 alpha와 opaque depth 검사. coverage 0은 조명 전 제외 | rear/front는 같은 draw 목록을 제출하되 각 fragment는 깊이 분할에 따라 한쪽에서 조명. 다층 refraction·교차 정렬의 일반 해법을 증명한 것은 아님 |
| Normals·tangents | [Surface VS](../src/Extends/AdvancedRendering/Shaders/surface_vs.glsl)의 scaled cofactor 변환을 Main과 통일. UV flip의 tangent handedness와 양면 방향 처리 | 기존 CalculateTangents를 사용하며 MikkTSpace 구현이 아님. 작은 비특이 변환의 절대 determinant 임계치 문제는 수정했으나 퇴화·ill-conditioned 입력은 추가 감사 대상 |
| Light | [Lighting](../src/dyf/Lighting.cpp), [cluster CS](../src/Extends/AdvancedRendering/Shaders/clustered_lighting_cs.glsl): 일반 광원 자료와 범위·cone, overflow 시 전체 목록 fallback | glTF KHR_lights_punctual 미지원. FBX 감쇠를 inverse-square 모델로 옮기는 과정은 근사이며 경고 대상. 모든 DCC의 광원 단위·렌더러와 일치한다고 주장하지 않음 |
| Camera | [Model API](../src/Public/dyf/Extends/Model/Model.h)에 glTF/FBX camera node·FOVy·clip·aspect·axes·orthographic height 보존. axes/FOV CPU fixture 통과. 원본 좌표·animation과 [viewer projection](cinematic/main.cpp)을 구분 | showcase의 크기 정규화·clip 범위는 명시적 viewer 설정. 카메라 프리셋 일치는 원본 렌더 동등성 증명이 아님 |
| Color·exposure | base/emissive의 색 공간, 데이터 texture의 선형 해석, [최종 tone mapping](../src/Extends/AdvancedRendering/Shaders/temporal_filmic.glsl) 분리 | ACES fitted·Reinhard·노출 차이가 최종 색을 바꿈. 입력 색상 일치만으로 최종 픽셀 일치를 보장하지 않음 |
| BRDF·IBL | [Main PS](../src/dyf/Shaders/mesh_ps.glsl), 투명 PS: 투명 IBL도 동일 LUT의 `F0*A+B`, roughness Fresnel, authored AO 규약 사용. IOR 기반 F0 유지 | GGX split-sum·SH·LTC·Schlick geometry는 실시간 근사. LUT 누락 수정이 완전한 다중 산란·스펙트럼 해법을 의미하지 않음 |
| SSR | [screen_space_ps](../src/Extends/AdvancedRendering/Shaders/screen_space_ps.glsl): confidence만큼 기존 environment specular를 대체 | 화면 밖·가려진 geometry 정보 없음. 유한 step, roughness 처리 및 denoise 근사 포함 |
| GI·AO | 같은 screen shader와 [denoise](../src/Extends/AdvancedRendering/Shaders/screen_denoise.glsl). 원본 AO와 screen AO는 간접광에 적용 | SSGI는 전체 장면의 unbiased GI가 아님. 조명·차폐·노출을 혼동하여 검은 영역을 특정 원인으로 단정하지 않음 |
| Shadows | [ShadowPass](../src/dyf/ShadowPass.cpp), [Model shadow VS](../src/Extends/Model/Shaders/mesh_shadow_vs.glsl): Mask texture와 정점 alpha 전달. 명시적 `stockShadowCoverage` 계약 | CSM·PCSS·bias·유한 해상도 및 blend shadow 한계 존재. custom shadow shader의 기존 동작을 자동 변경하지 않음 |
| Instancing·상수 재사용 | stock shader·instance stream·`IndirectFirstInstance` 지원 조건에서만 offset 0/firstInstance. [ForwardPass](../src/dyf/ForwardPass.cpp)의 stock VS/PS 조건부 상수 재사용 | custom override와 미지원 장치는 기존 경로 유지. 자원·draw 순서·가시성 보존은 fixture와 backend 실행으로 확인 |
| Sync·수명 | [VulkanDevice](../src/Backends/Vulkan/VulkanDevice.cpp)의 acquire/present 재획득 조건, [ICommandList](../src/RHI/ICommandList.cpp)의 기록 캐시 | unchanged suboptimal과 실제 surface 변경을 구분. 한 플랫폼의 성공을 모든 WSI·실패 경로의 증명으로 확대하지 않음 |
| Backend | GLSL/HLSL/Metal에 동일한 자료·수식·coverage 변경 반영 | 소스 대조와 컴파일, 실제 GPU 실행은 별개의 증거. 현재 실기 증거는 Linux Vulkan 환경에 한정 |
| Mobile | 기능·format 지원 gate와 기존 경로 fallback. R16F reactive는 RT+blend+sampled 지원이 입증될 때만 선택 | tile memory·대역폭·shader register·실제 allocation은 기기별 측정 필요. 모바일 1080p 60 FPS 보장 없음 |

## Emerald 원본과 재질 해석

- source inspector의 삼각형 수는 **10,046,405**이며 ORCA README와 일치한다.
- Blender 4.5.14의 Y-up→Z-up 변환 후 bounds와 **220개 material**이 비교 결과와 일치했다. 원본의 canonical duplicate polygon/triangle은 unique mesh 기준 **4,035개**, instance 가중 **50,337개**다. `10,046,405−50,337=9,996,068`로 Blender 삼각형 수와 정확히 일치한다. 원본 index 퇴화는 0개이며 별도 zero-area 삼각형의 가중 23개는 이 차이의 원인이 아니다. 엔진은 원본 duplicate geometry를 보존한다. 이 집계 일치를 전체 지오메트리·렌더 동등성으로 확대하지 않는다.
- 원본 camera의 FOVy는 약 **33.9666°**, near/far는 **.001/6000**이다. 기존 viewer FOV와 원본 camera 의미를 구분하여 보존하며, 후속 이미지 비교에는 사용한 camera 설정을 기록한다.
- 조사한 식생의 raw `Opacity=1`, `TransparentColor=0` 및 기존 T=0을 확인했다. 따라서 T가 alpha를 덮어쓰던 일반 결함을 이 식생의 직접 원인으로 설명하지 않는다.
- WhiteOak/Maple texture에 중간 alpha가 각각 `261,861/4,194,304`, `237,080/4,194,304` texel 존재하여 기존 자동 Blend 분류가 확인되었다. 중간 alpha만으로 원저자의 최종 raster alpha 정책을 역추정할 수는 없다.
- WhiteOak의 covered texel RGB 평균은 약 `(94.684, 116.391, 77.628)`의 8-bit 값이었다. 원본 색상은 흰색이 아니며, 이 평균을 선형 반사율이나 최종 조명값으로 해석하지 않는다.
- 원본의 vertex-colored instance 수는 0이었다. COLOR_0 누락 수정은 범용 결함 수정이며, Emerald 흰 식생의 직접 원인이라는 증거는 아니다.
- [Falcor scene 형식 문서](https://raw.githubusercontent.com/NVIDIAGameWorks/Falcor/master/docs/usage/scene-formats.md)는 metal-rough Specular texture의 R을 occlusion으로 표기하면서 **unsupported**라고 명시한다. ORCA profile은 SpecR을 실제 AO로 사용하지 않는 해석과 명시적인 cutout 정책을 선택한다. 이를 다른 FBX 재질의 보편 규칙으로 삼지 않는다.
- [Falcor `BasicMaterial::updateAlphaMode`](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/Scene/Material/BasicMaterial.cpp)는 alpha 범위와 threshold로 Mask/Opaque를 결정한다. 이는 import profile 선택의 근거이며 모든 렌더러가 같은 정책을 사용한다는 증거는 아니다.

수정 전 [화면](../build/showcase/emerald-review.png)과 수정 후 [같은 구도의 화면](../build/showcase/emerald-corrected-same-view.png)을 남겼다. 둘 다 장면 15초, 기존 FOV 50°, 같은 조명·노출·후처리를 사용한다. [수정 후 캡처 조건](../build/showcase/emerald-capture-conditions.txt)을 보존했고, 최종 성능 측정의 원본 FOV 33.9666°와 구분한다. 수정 후 녹색 잎과 그늘의 정보가 보인다. 이 비교는 모델 색을 맞추기 위한 노출 변경이 아니며, 원저자의 Falcor PBR 전체 렌더와 픽셀 일치를 증명하는 비교도 아니다.

## 독립 참조·자원 수명 검사

[BlenderReference.py](../src/Extends/Model/tests/BlenderReference.py)는 같은 `VertexColor.gltf`를 factory 설정의 Blender 4.5.14 CPU Cycles로 렌더한다. 선형 COLOR_0를 emission으로 전달하고 양쪽에 동일한 Reinhard·sRGB 출력을 사용한다. 삼각형 경계의 AA 차이를 제외한 내부 14,196픽셀 중 11,641개가 RGB까지 같았고, 최대 차이는 1 byte, 평균 절대 차이는 0.06373 byte였다. [수치 기록](../build/showcase/vertex-color-reference-result.json), [Blender 이미지](../build/showcase/vertex-color-blender.png), [엔진 이미지](../build/showcase/conformance-images/imported-model.png)를 남긴다. 이 비교는 geometry 배치·보간·색상 전달의 증거이며, GGX·GI·그림자의 동등성 증거는 아니다.

GPU fixture가 실제로 드러낸 별도 결함 두 가지도 수정했다. BasicEnvironment의 중립 probe fallback은 `sampler2DArray` 계약에 맞는 2-layer texture를 사용한다. Vulkan은 Model 사전 compute 제출이 반복되어 swapchain resize를 영원히 미루던 조건을 제거했다. resize의 예외 경로에서만 기존 host present·GPU 제출 완료를 기다린 뒤 자원을 교체하며, 일반 프레임에 대기를 추가하지 않는다.

Wine 원본의 `TransparentGlass` 세 texture는 일부 mesh에 없는 `map1`을 참조한다. 해당 mesh에는 서로 다른 `TextureUV`와 `LightMapUV`가 있어 자동 alias는 근거가 없다. 같은 재질을 쓰면서 `map1`이 실제 첫 UV로 존재하는 다른 mesh도 있다. 공개 `ModelMaterialOverride.fbxFallbackTextureUvSet`으로 **존재하지 않는 참조에만** 실제 첫 UV를 명시하도록 하고, ORCA preset에서 이 재질에 `TextureUV`를 지정했다. 유효한 원본 UV0 참조는 그대로 사용한다. BaseColor·Specular는 전체 5 mip, Normal은 단일 1×1 mip의 활성 texel이 상수임을 실제 decode로 확인했다. 따라서 이 선택은 세 texture의 샘플 값을 바꾸지 않는다. Normal `(127,127,255)`는 비중립 값이라 삭제하지 않았다. 잘못된 이름·실제 두 번째 UV 선택은 명시 fallback을 켜도 거부하며 mixed-mesh CPU fixture와 공개 model cache로 확인했다. 이는 해당 원본의 명시적인 복구 설정이며, 일반 FBX 로더에 이름 추론을 넣은 것이 아니다.

## NativeMoments 수치 반례와 조치

기존 native 경로의 finite·범위·format 지원 검사는 overflow 가능성을 제한하지만, 수동 FP32 보간과의 작은 visibility 오차를 보장하지 않는다. [Vulkan의 `subTexelPrecisionBits`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html)는 보간 좌표가 제한된 fractional grid에 snap될 수 있음을 명시한다. `m2 - m1²`로 계산하는 분산은 이 차이를 증폭할 수 있다.

메모리에서 실행한 FP32 스칼라 모델의 입력은 normalized 거리 `d0=.25`, `d1=1`, 2차 moment `d²`, receiver `.30`, texture 높이 `128`, 한 축의 보간 weight `1/1024`였다. 다른 축은 일정하게 두었다.

| 값 | 수동 FP32 보간 | weight를 8 fractional bits로 반올림한 모델 |
|---|---:|---:|
| weight | 0.0009765625 | 0 |
| 평균 거리 | 0.25073242 | 0.25 |
| shader variance floor 적용 후 분산 | 0.000548780 | 0.0000376496 |
| visibility | 0.2279393 | 0.0191922 |

절대 visibility 차이는 **0.208747**이다. 입력 moment는 `[0,1]` 범위여서 기존 finite·magnitude 검사를 통과한다. 이 결과는 **정밀도 모델의 반례**이며 실제 RTX 3060 또는 모바일 GPU 출력값이 아니다.

추가 모델에서 높이 256, 각 bit 수의 quarter-step weight를 사용하면 8/10/12 fractional bits의 절대 차이는 각각 약 `.200334/.054861/.011123`이었다. 같은 weight를 사용한 정밀도 수렴 표로 해석하지 않는다.

조치: [probe_visibility 3개 backend](../src/Extends/AdvancedRendering/Shaders/probe_visibility.glsl)를 항상 기존 4탭 FP32 보간으로 실행하고 NativeMoments 전용 gate·CPU 검사·sampler 전달을 제거했다. Native HDR radiance 필터링은 별도 경로로 유지하되, 수동 보간과 bitwise 동등하다고 주장하지 않는다.

## 모바일 메모리·실행 부담

아래는 **1920×1080**, AO+GI+SSR+환경 diffuse/specular+2층 투명+bloom 활성, TAA 비활성일 때 소스에서 계산한 texture payload다. 실제 GPU allocation/RSS가 아니며 geometry·material texture·probe array·swapchain·upload staging·alignment·driver overhead를 제외한다.

| 렌더 texture 집합 | MiB |
|---|---:|
| Surface normal/albedo/motion + resolve | 63.28 |
| half-resolution raw AO/GI/SSR | 19.78 |
| GI/SSR denoise history·spatial targets | 79.10 |
| R32 depth plane | 7.91 |
| environment diffuse/specular 출력 | 31.64 |
| 2층 투명 color/reactive/rear/depth | 63.28 |
| main HDR + depth | 23.73 |
| bloom mip targets | 10.54 |
| **합계: 기존 RGBA16F reactive 기준** | **299.26** |
| R16F reactive 선택 시 절감 | **−11.865** |
| TAA 활성 시 추가 | +31.64 |
| High shadow atlas 상한: 8192² D32 | +256.00 |
| Medium shadow atlas 상한: 4096² D32 | +64.00 |
| global 4096×2048 RGBA32F radiance + 전체 prefilter mips | +298.67 |

기존 RGBA reactive·High shadow·4K HDR 조합은 위 제외 항목을 넣기 전 약 **853.93MiB**다. shadow atlas는 장치 dimension 한도로 제한되며, 표는 실제 장치의 allocation 결과가 아니다. TAA나 효과를 끈 뒤 이전 target이 유지되는 경우도 있어 active 설정만으로 전체 상주 메모리를 단정할 수 없다.

R16F reactive는 R 채널의 기존 half 정밀도와 fragment alpha 기반 blend 식을 유지한다. [생성 코드](../src/Extends/AdvancedRendering/TransparentRendering.cpp)는 RT·blend·independent write mask와 sampled 지원을 확인한 경우에만 사용한다. 필요한 proof가 없으면 RGBA16F로 돌아간다. 소비자는 [TAA](../src/Extends/AdvancedRendering/Shaders/temporal_resolve.glsl)의 `.r` 읽기다. GPU fixture는 실제 attachment format과 최종 RGB를 확인한다. public readback이 RGBA8/BGRA8만 지원하므로 half 채널의 직접 bitwise 비교는 하지 않았다.

정점 RGBA를 복원하면서 stock GPU vertex stride는 48→64 byte로 늘었다. 따라서 정점당 16 byte의 추가 저장·fetch 비용도 있으며, 위 reactive 절감을 전체 VRAM 순감소로 해석하지 않는다. 색상이 없는 mesh의 기본 흰색을 별도 stream/layout으로 전달하는 최적화는 후속 과제다. 현재는 원본 색상을 양자화하거나 누락시켜 이 비용을 숨기지 않는다.

[RHI Format](../src/Public/dyf/RHI/Format.h)에 `R16_FLOAT`를 정식 추가하고 공통·Null·Metal 검증의 2-byte 크기, 1-channel clear, Vulkan/D3D12/Metal native format 및 필요한 filtering query를 연결했다. 기존 enum 값은 유지한다. RGBA8 전용 public readback·swapchain·index format 계약은 확장하지 않았다. Metal의 R16Float 지원 근거는 [공식 feature 표](https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf)의 pixel format capability이며, Metal 실기 검증 증거는 아니다.

Probe `maxReflectionResolution=0`은 원본 해상도 보존이며, 배열은 radiance와 moment용 총 `2N` layer를 사용한다. 소스상 실행 부담에는 투명 overdraw·2층 제출, PCSS tap, half-resolution GI의 4개 ray×최대 16 step, SSR 최대 32 step, denoise MRT가 포함된다. 실제 시간·대역폭·register 사용량은 모바일 실기 측정 없이 수치로 보장하지 않는다.

실제 Emerald 업로드 로그의 material texture payload는 **2,994,748,932 byte**이며 336개 이미지 중 225개에서 지원되는 원본 BC1/BC3 mip를 보존했다. RGBA equivalent는 **6,643,826,256 byte(약 6.19GiB)**다. 이는 texture payload 집계이고 driver allocation은 아니다. BC를 사용할 수 없는 모바일의 RGBA fallback에 이 대형 asset을 그대로 넣을 수 있다고 판단하지 않는다. 모바일은 native 압축 format·texture/mip 예산·streaming과 render target/shadow 예산을 별도 설계해야 한다. Android/iOS 실기·SDK 환경이 없어 빌드·FPS·메모리는 측정하지 않았다.

## 미지원·근사·후속 확인

- `ModelLoadOptions.allowMaterialFallbacks`는 기본 false다. UV set 0 이외의 참조, 비항등 KHR_texture_transform, 미지원 glTF sampler wrap/filter, transmission/thickness texture·미지원 material 확장은 기본 실패한다. 명시 true일 때만 경고와 근사를 허용한다. 누락 UV·FBX transform·wrap·dangling UV 이름·height bump의 normal 해석도 검사한다. 상세 조건은 [glTF](../src/Extends/Model/GltfModelLoader.cpp), [FBX](../src/Extends/Model/UfbxModelLoader.cpp)에 있다. 일부 물리량의 근사·전체 material graph 미지원 자체를 없앴다는 주장은 아니다.
- glTF KHR_lights_punctual과 FBX CullingOnCW의 전면 방향 선택은 미지원이다. FBX의 geometry·pivot·animation은 ufbx 변환 결과를 사용하고, source axis/unit는 별도 metadata로 제공한다. 이 감사는 glTF 전체 conformance 인증이 아니다.
- 스크린 공간 GI/SSR, shadow map, SH·GGX prefilter·LTC, finite sample count, tone mapping은 명시적인 실시간 근사다. path tracer·Blender/Falcor/Unreal/Unity의 전체 material graph와 동등하다고 주장하지 않는다.
- 원본 mesh 데이터 보존과 실행 중 LOD 선택은 구분한다. Showcase는 projected error 1 pixel의 LOD를 기본 사용하며, 전체 형상과 bitwise 동일한 화면을 보장하는 설정은 아니다. 독립 색상 fixture는 LOD를 끄고 비교했다. showcase의 크기 정규화·카메라 시간 조정·선택적 wind·시연 조명도 원본 asset 파일의 변경과 구분한다.
- Vulkan 실행, HLSL/Metal 소스 대응, 플랫폼별 컴파일, Android/iOS 실기는 각기 다른 검증 상태로 기록한다.
- 정점 RGBA 추가로 public/stock vertex layout이 바뀌었다. dependent 코드의 재빌드와 custom shader layout 계약 점검이 필요하다. 기존 custom shadow는 명시적인 opt-in 없이 stock coverage를 적용하지 않는다.
- 임의 asset 이름별 셰이더 분기나 고정 camera로 결과를 맞추지 않는다. 공개 import 옵션·자료의 의미·기능 지원 여부를 경계로 사용한다.

## 성능 결과

RTX 3060 12GB, NVIDIA 615.71.10, Debian 13·Vulkan·격리 headless Sway에서 측정했다. 내부·출력 모두 **1920×1080**, VSync/HUD/validation/capture OFF, warm-up 120회 후 GPU drain, 성공 제출당 장면 시각 1/60초다. 최종 GPU drain을 포함한 처리량이며 display presentation FPS가 아니다. Cinematic은 High shadow·투명 2층·TAA OFF이고 각 프리셋의 조명·노출·GI 설정은 유지했다. 원본 FBX camera FOVy는 약 33.9666°이며 showcase clip·시간 정규화는 별도 viewer 설정이다.

| 장면 | 완료 제출/s | 간격 P95 / P99 ms | Renderer CPU ms | GPU graph ms | 구간·원시 CSV |
|---|---:|---:|---:|---:|---|
| Night | 60.7348 | 18.8371 / 19.5137 | 7.2238 | 16.4416 | 3,600회·0–60초, [CSV](../build/showcase/night-conformance.csv) |
| Exterior | 77.8292 | 14.7896 / 15.8173 | 7.0593 | 12.8242 | 3,600회·0–60초, [CSV](../build/showcase/exterior-conformance.csv) |
| Interior / Wine | 160.7127 | 8.8833 / 11.0116 | 4.5590 | 6.2093 | 3,600회·0–60초, [CSV](../build/showcase/interior-conformance.csv) |
| Emerald | 46.1598 | 28.4281 / 30.2785 | 6.6526 | 21.6648 | 1,200회·15–35초, [CSV](../build/showcase/emerald-conformance.csv) |
| Sponza | 86.6228 | 12.8502 / 14.7583 | 3.4481 | 11.5546 | 1,200회·0–20초, [CSV](../build/showcase/sponza-conformance.csv) |

CPU/GPU 값은 각각 profiler publication의 sample 수로 가중한 평균이다. 간격 percentile은 CPU 대기·재시도를 포함한 제출 간격이며 GPU percentile이 아니다. Night는 평균 60–70 목표에 도달했지만 P95가 16.67ms보다 커서 일정한 60 FPS는 아니다. 기존의 FOV 50°·다른 출력 크기·잘못된 재질 해석 결과와 직접 속도 개선율을 계산하지 않는다.

| 장면 | GPU 주요 패스 평균 ms |
|---|---|
| Night | Shadow 5.0880, Main 4.4342, Surface 1.9184, GI ray 1.5237 |
| Exterior | Shadow 4.1910, Surface 1.8785, Main 1.8731, GI ray 1.4860 |
| Wine | 투명 rear .9690, Main .8231, Surface .6844, 투명 front .5644 |
| Emerald | Shadow 10.1327, Main 3.8578, Surface 3.2590, GI ray 1.3319 |
| Sponza | Shadow 3.6882, GI ray 1.6283, Main 1.6097, Surface .9920 |

Emerald의 현재 주요 비용은 그림자다. [중복 분석 로그](../build/showcase/shadow-duplicate-audit.log)는 실제 imported pos·UV·alpha 비트가 같은 shadow triangle을 조사했다. 제거 가능한 잠재 비율이 Emerald .4974%, Bistro .0761%, Sponza 0%여서 구현 범위를 늘리지 않았다. normal/UV/material 차이가 있는 원본 면을 임의로 지우지 않는다. 수치는 instance 가중 geometry 집계이며 shadow view별 가시성 집계는 아니다.

### Arena의 CPU 중복 계산

최적화 전 동일 1080p·72 skeleton+1 knight·60초 경로에서 **31.3742 제출/s**였다. 전체 frame work 31.8580ms, ModelRenderer 호출 27.6862ms, Renderer CPU 4.4892ms, GPU graph 9.7645ms다. VSync/HUD가 꺼져 있어 이 결과를 VSync 탓으로 설명할 수 없다. ModelRenderer가 compute 출력의 정확한 bounds를 구하기 위해 CPU에서도 각 정점의 skin matrix를 반복 구성하는 경로를 확인했다.

행렬은 mesh의 influence와 해당 entity palette에만 의존한다. binary 동일 influence를 그룹화하여 행렬을 재사용하되, 모든 정점의 기존 TransformPoint·bounds 누적 순서·geometry signature·history는 유지한다. 전부 고유한 influence이면 기존 경로를 사용하고 공개 `cacheSkinTransforms=false`로 같은 입력의 비교가 가능하다. conservative joint AABB는 shadow fit·투명 정렬을 바꿀 수 있어 채택하지 않았다.

CPU 회귀는 512정점·35종 influence와 8개의 변하는 palette로 LBS·DQ·mixed·반사 scale의 그룹/정점별/전체 SkinVertex 위치와 bounds를 bitwise 비교했다. zero/tiny-total signed-zero 복사, invalid weight·joint·DQ 거부 및 그룹 재구성도 검사했다. GPU 35-frame 회귀의 advanced OFF/ON 전체 이미지 차이 0과 함께 확인했다. shader·GPU 변형 결과·해상도·모델 수·광원 품질을 바꾸지 않는 CPU 중복 계산 절감이다.

같은 실행 파일에서 `--benchmark-skin-cache off|on`만 바꾸어 120회 warm-up·3,600회 측정·60초의 동일 경로를 순차 실행했다.

| 항목 | OFF | ON |
|---|---:|---:|
| GPU drain 포함 완료 제출/s | 29.9786 | **45.6353** |
| 제출 간격 평균 ms | 33.3546 | 21.9110 |
| 제출 간격 P95 / P99 ms | 34.1388 / 34.9759 | 22.5996 / 23.0816 |
| 전체 frame work ms | 33.3427 | 21.9007 |
| ModelRenderer 호출 wall ms | 29.1795 | 17.7367 |
| Renderer CPU ms | 4.3837 | 4.3588 |
| Renderer GPU graph ms | 9.9631 | 8.4418 |

처리량은 **52.23% 증가**했다. [OFF CSV](../build/showcase/arena-skin-cache-off.csv), [ON CSV](../build/showcase/arena-skin-cache-on.csv)를 보존했다. 3,600프레임 모두 scene time·main/shadow draw·shadow triangle·indirect batch·가시성 설정·shadow reuse·render/output 크기가 일치했다. 전체 경로의 매 프레임 이미지를 모두 readback한 비교는 아니며, 실제 픽셀 동등성은 위 35-frame fixture에서 확인했다. GPU clock을 고정하지 않았으므로 GPU 시간 차이를 이 CPU 최적화의 GPU 연산 절감으로 해석하지 않는다.

Arena는 Medium shadow·투명 1층·AO/fog/bloom ON·GI/SSR/TAA OFF라는 자체 기본 품질을 유지한다. shader·geometry·품질을 낮추지 않았으며 현재도 CPU의 정확한 변형 경계 평가·애니메이션 등이 남는다. 다른 Cinematic 장면과 같은 작업량의 FPS 대결로 해석하지 않는다.

## 기준 문서

- [Khronos glTF 2.0: 재질·색 공간·alpha·double-sided 규약](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
- [KHR_materials_transmission: coverage와 광학 투과의 분리](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_transmission/README.md)
- [KHR_materials_volume: thin surface와 volume의 구분](https://raw.githubusercontent.com/KhronosGroup/glTF/main/extensions/2.0/Khronos/KHR_materials_volume/README.md)
- [Filament: BRDF·split-sum IBL·occlusion 및 근사 한계](https://google.github.io/filament/main/filament.html)
- [Blender glTF 설명: alpha coverage와 transmission](https://docs.blender.org/manual/en/3.6/addons/import_export/scene_gltf2.html)
- [Falcor scene formats](https://raw.githubusercontent.com/NVIDIAGameWorks/Falcor/master/docs/usage/scene-formats.md), [BasicMaterial alpha 정책](https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/Scene/Material/BasicMaterial.cpp)
- [Vulkan interpolation precision](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html), [framebuffer blending](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html)

기준 문서의 모델을 참고했다는 사실과 해당 프로그램에서 같은 장면을 실제 렌더해 비교했다는 사실을 구분한다. 후자의 결과가 없으면 참조 렌더 검증 완료로 기재하지 않는다.
