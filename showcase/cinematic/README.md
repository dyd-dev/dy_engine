# Cinematic Showcase

카메라 오류를 수정하고 실제 Bistro 모델로 조명과 재질을 확인했다. 종전에는 단위가 다른 FBX Camera의 legacy Position을 노드 좌표로 사용해 모델 밖에서 HDRI 배경 사진만 보고 있었다. Wine의 카메라 좌표는 실제 노드 위치와 100배 차이가 났다. 현재는 모델과 같은 노드 계층으로 카메라를 계산한다.

TAA 입력은 geometry와 하늘을 같은 jitter sample grid에 렌더링한다. 하늘은 translation을 제외한 카메라 회전으로 재투영하며 정지 시 정확한 identity를 사용한다. 윤곽에서 motion을 선택할 때 현재·이전 depth도 같은 표면의 값으로 맞춘다. 종전의 jitter 없는 배경과 jitter 있는 geometry 혼합을 제거한 변경이며 실행 확인은 사용자에게 남아 있다. 실제 모델에서 검게 뭉치던 그늘은 Bistro Specular 텍스처 201개의 R 채널이 모두 0인데 이를 AO로 연결한 것이 원인이었다. 예제에서 해당 채널의 occlusionStrength를 0으로 두고 G/B roughness·metalness와 화면 공간 AO는 유지한다. 외부의 환경광 비율·노출과 실내 광원을 수정한 뒤 Wayland/Vulkan 실행 화면에서 벽·목재·가구와 그늘 바닥이 보이는 것을 직접 확인했다. 캡처는 `/home/suhan/dev/assets/bistro-quality-review-20261017`에 저장했다. 유리의 작은 흰 하이라이트에는 화면 내 법선 분산에 따른 specular AA를 적용했다. GGX 반사 폭을 픽셀의 법선 변화에 맞춰 필터링하고 peak 부근 분모의 정밀도 손실을 줄인다. 공통 GLSL/HLSL/Metal 셰이더를 수정했고 Vulkan 재빌드·실내 실행·연속 캡처를 수행했다. 모든 카메라·이동 상태에서 깜빡임이 완전히 사라졌다고 확정하지 않는다.

`BistroInterior_Wine.fbx`에 없는 점광원 4개는 `LoadModelLights()`로 일반 `BistroInterior.fbx`에서 읽어 같은 좌표계로 연결한다. 이 경로는 geometry·texture·animation을 디코드하지 않는다. 위치와 range는 정규화 scale, inverse-square intensity는 scale²로 변환한다. Wine에는 외부용 태양을 추가하지 않고 야외 HDRI도 사용하지 않는다. 현재 노출은 실내 0.12, 외부 0.04이며 외부 환경광 intensity는 원본 설정 10이다. 공통 렌더러 기본값은 바꾸지 않는다.

이번 조명 변경은 실내 시작 시 생성하는 정적 local probe와 투명 직접광의 그림자 차폐다. 첫 원본 카메라 위치에서 한 면 256×256의 6방향 HDR 래스터 캡처를 두 벌, 총 12개 view로 생성하고 각각 1024×512 panorama로 저장한다. 반사용은 발광 표면을 포함하고, 확산광용은 직접 보이는 발광 표면을 제외해 작고 밝은 램프가 diffuse SH를 과도하게 지배하는 경로를 줄인다. 반사는 거칠기별 필터링과 실제 geometry 월드 AABB 기반 위치 보정을 사용하고 확산광은 SH로 압축한다. opaque/masked geometry만 캡처하며 투명 유리·액체와 후처리는 포함하지 않는다. 매 프레임 다시 캡처하지 않으므로 준비 시간은 늘지만 정상 프레임마다 12개 view를 추가하지 않는다. 실내 SSGI는 probe와 직접광의 중복·밝은 샘플의 불안정·프레임 비용을 줄이기 위해 기본 OFF로 두며 8키로 켤 수 있다. 두 실행 파일의 Linux Wayland/Vulkan 빌드는 완료했고 이 변경의 프로그램 실행·실제 화질·FPS·초기 준비 시간 확인은 하지 않았다.

유리 밖에서도 흰 점 깜빡임이 보고되어 Main과 Surface의 GLSL/HLSL/Metal 6개 셰이더에도 같은 specular AA를 적용했다. Surface에 필터링된 roughness를 저장해 IBL·SSR의 반사 폭을 Main과 맞췄으며 Vulkan 컴파일·링크를 완료했다. 이 추가 변경의 실행 결과는 아직 확인하지 않았다. 잔여 흰 점 전체가 해결됐다고 취급하지 않는다. 별도 원인 후보로 SSGI의 4-ray hit가 밝은 발광 램프를 간헐적으로 샘플하는 고분산 경로가 있다. GI 전용 시간 누적·모멘트·공간 노이즈 필터는 아직 구현하지 않았고, 최종 TAA에 의존한다. SSR의 고정 5점 HDR 샘플도 밝은 표면에서 같은 종류의 불안정성이 가능하다. 해당 경로가 실제 잔여 점의 원인인지는 효과별 화면 비교 전이다.

환경 시연은 **Bistro의 빛·재질·유리와 Emerald Square Day의 광역 가시성·LOD**로 구성한다. 기본 장면은 Bistro 외부 거리이며 Wine 실내와 Emerald를 선택한다. 반복 객체 장면은 별도 [Dense Arena](../arena/README.md), Sponza는 개발 회귀·효과 비교용이다. Android/iOS는 별도 브랜치 작업이다.

공통 렌더러에는 FBX/glTF 공유 geometry와 화면 오차 기준 indexed LOD를 연결했다. LOD는 실제 index buffer를 바꾸며 색상 표시만 바꾸는 예제가 아니다. Emerald 연결까지 포함한 `ShowcaseCinematic`·`ShowcaseArena` 두 target의 Linux Wayland/Vulkan 최종 빌드를 완료했다. 프로그램 실행·구도·화질·성능 측정은 하지 않았다. 광역 에셋은 원본 FBX를 지원하는 Emerald Square로 선택했으며 포맷 변환은 하지 않는다. Jungle 원본은 보관하지만 Blender/USD 로더가 없어 현재 영상 장면에서 제외한다.

[main.cpp](main.cpp)는 기존 ModelScene·Renderer·Camera·Window를 사용한다. 효과와 투명 렌더링은 공통 Renderer가 공개 RHI로 구성하며 backend는 네이티브 명령으로 번역한다. 현재 개발 환경은 Linux Wayland/Vulkan이며 Windows D3D12·Windows Vulkan·Android Vulkan·iOS Metal을 최종 타깃으로 한다. 셰이더 소스 제공, 플랫폼 빌드, 실제 기기 실행은 각각 구분한다.

`build/showcase`는 Wayland/Vulkan Release 산출물이다. Windows D3D12·Windows Vulkan·Metal 빌드와 기기 실행은 수행하지 않았다. 투명 표면의 coverage는 별도 reactive MRT에 기록하되, coverage만으로 정적 유리의 history를 매 프레임 버리지 않고 현재·이전 HDR의 상대 색 차이를 함께 반응도로 사용한다. 주변에서 선택한 motion으로 재투영할 때 깊이 비교도 같은 motion의 이전 깊이에 맞춘다.

밉맵 준비는 최대 4개 임시 CPU 작업으로 제한하고 GPU 업로드는 이미지 한 장씩 처리한다. `textureDecodeThreadCount=4`는 glTF의 지연 외부 이미지 디코딩에 적용되며 FBX 로더 내부의 이미지 디코딩은 현재 순차 처리한다. DDS도 현재 GPU에는 RGBA8로 올리므로 모바일용 압축 텍스처 저장·품질 예산은 별도 브랜치에서 연결할 작업이다.

## 에셋과 선택

[NVIDIA ORCA Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) 배포본은 /home/suhan/dev/assets/Bistro_v5_2에 압축 해제했다. 번들의 LICENSE.txt는 CC BY 4.0이며 Amazon Lumberyard/ORCA 출처와 수정 내역을 영상 크레딧에 남긴다. 외부 약 283만, 와인 내부 약 129만 삼각형은 배포본 README의 전체 삼각형 수다. 실행 중 실제 제출량과 동일한 수치로 취급하지 않는다.

| 실행 인자 | 장면 |
| --- | --- |
| 생략 | CMake의 DY_SHOWCASE_SCENE. 새 구성 기본값은 BistroExterior.fbx |
| exterior | 기본 장면 파일과 같은 폴더의 BistroExterior.fbx |
| interior | 같은 폴더의 BistroInterior_Wine.fbx |
| emerald | CMake의 DY_SHOWCASE_EMERALD_SCENE. EmeraldSquare_Day.fbx 원본 |
| 파일 경로 하나 | glTF·GLB·FBX·OBJ 입력. Bistro·Sponza 외에는 Y-up 장면용 바깥쪽 궤도 |

[NVIDIA ORCA Emerald Square](https://developer.nvidia.com/orca/nvidia-emerald-square)는 `/home/suhan/dev/assets/EmeraldSquare_v4_1.zip`(617,345,248B)을 받아 `/home/suhan/dev/assets/EmeraldSquare_v4_1`에 추출했다. 362개 항목·1,544,054,516B이며 원본 `EmeraldSquare_Day.fbx`·`EmeraldSquare_Day.pyscene`·`symmetrical_garden_4k.hdr`를 그대로 사용한다. CMake `DY_SHOWCASE_EMERALD_SCENE`은 이 FBX, `DY_SHOWCASE_EMERALD_ENVIRONMENT`는 이 HDR를 선택한다. 포맷 변환·에셋 재생성 경로는 없다.

배포 README의 unique triangles 2,695,054·total triangles 10,046,405는 원본 장면 수치이며 실제 프레임 제출량과 다르다. 번들 라이선스는 CC BY-NC-SA 3.0이다. 영상 크레딧에 Nicholas Hull·Kate Anderson·Nir Benty, NVIDIA ORCA, 식생의 SpeedTree 크레딧과 수정 내역을 남긴다.

Emerald는 광역 장면 후보이며 LOD·모바일 대표 장면으로의 적합성은 미확인이다. 전체 삼각형 수만으로 선정하지 않고 현재 동선의 근중원경 분포, 실제 생성·선택 단계와 제출량 감소, 정상 렌더의 실루엣, 목표 기기의 메모리·프레임 비용을 기준으로 판단한다. 와이어프레임이 모두 LOD0였다는 사용자 보고와 FBX LOD 입력 수정 이후의 결과를 구분하며, 수정만으로 장면의 적합성을 확인했다고 취급하지 않는다.

Jungle 원본 약 7.44GB는 `/home/suhan/dev/assets/JungleRuins_1_0_1b`에 추출해 보관한다. 원본 USD가 참조하는 [Poly Haven Kloppenheim 06 Pure Sky](https://polyhaven.com/a/kloppenheim_06_puresky)의 2K HDR도 `/home/suhan/dev/assets/JungleRuins_1_0_1b/kloppenheim_06_puresky_2k.hdr`에 내려받았으며 원본 USD의 8K 참조는 수정하지 않았다. 출처는 Greg Zaal·Jarod Guest, 라이선스는 CC0다. 원본 추출·HDR 다운로드는 엔진의 geometry·재질 로딩이나 시연 통합 완료를 의미하지 않는다.

장면과 Textures 폴더는 원래 배치를 유지한다. 외부 HDRI는 번들의 san_giuseppe_bridge_4k.hdr이며 .pyscene에 명시된 intensity 10을 사용한다. Wine은 실내 점광원과 원본 스크립트의 발광 보정 1000배를 사용한다. 직접 조명은 복원한 점광원이 담당하고, 실내 표면에서 반사된 빛은 초기 local probe로 근사한다. 발광 geometry가 면광원으로서 방 전체를 직접 비추거나 다중 반사 GI를 계산하는 경로는 아니다. 외부 HDRI 로딩 실패 시 오류를 출력하고 종료한다. 기존 CMake 캐시에 다른 경로가 저장되어 있으면 새 기본값으로 자동 변경하지 않는다.

Bistro의 DDS 텍스처를 디코딩하고 BaseColor의 alpha를 opacity, Specular의 G/B를 roughness·metalness로 연결한다. 배포본의 Specular R은 모든 텍스처에서 0으로 비어 있어 예제에서 authored AO를 비활성화한다. DirectX normal map은 Y 부호를 -1로 전달한다. 이 convention은 Bistro 프리셋에만 적용하며 공통 기본값을 변경하지 않는다. .pyscene은 실행하지 않는다.

Emerald의 ORM은 R=AO·G=roughness·B=metallic이며 DirectX normal과 BaseColor alpha를 연결한다. Bistro의 비어 있는 AO 보정 `occlusionStrength=0`은 Emerald에 적용하지 않는다. 원본 FBX의 embedded camera·light와 HDRI intensity 1을 사용한다. Emerald 프리셋은 `advanced.autoExposure=true`로 화면의 로그 평균 휘도를 GPU에서 측광하고 ACES 전에 노출을 적용한다. 공통 기본값은 OFF이며 Bistro의 수동 노출은 유지한다. `exposureKey=0.18`, `exposureAdaptationSpeed=1.5`를 사용하고 -/+는 노출 보정값이다. 원본 Falcor의 자동 노출 누락을 보완한 것이며 서로 다른 톤매퍼의 동일 출력은 보장하지 않는다. 카메라·광원 포함은 배포 CHANGELOG v4의 구성이다. 원본 `.pyscene`을 실행하거나 다른 포맷을 생성하지 않는다.

TransparentGlass·TransparentGlassWine은 IOR 1.55, Water·White_Wine·Red_Wine·Beer는 1.33, Ice는 1.31과 transmission 1을 적용한다. 해당 유리·액체는 원본처럼 metallic·roughness를 0으로, Ice의 roughness는 0.1로 지정한다. Bistro의 legacy diffuse tint와 DiffuseFactor는 투과색으로 쓰지 않고 중립색을 적용한다. 색은 배포본 Beer–Lambert 흡수 계수로 처리한다. 유리 3mm·액체 4cm·얼음 3cm의 고정 두께를 근사값으로 두고 장면 정규화 비율에 맞춰 두께·흡수 거리를 함께 환산한다. 공통 투명 셰이더는 양면을 가시 법선에 정렬하고 air→IOR의 thin-wall 근사로 굴절시켜 뒤면의 허위 전반사가 배경을 검정으로 덮지 않도록 한다. 원본 Falcor의 nested priority·체적 경계·실제 물체별 두께를 재현하는 경로는 아니다.

## 카메라

장면은 중심을 맞추고 가장 긴 축을 80단위로 정규화한다. ufbx는 원본 축을 유지하므로 Bistro·Emerald·Sponza의 Y-up을 예제에서 한 번만 Z-up으로 회전한다. Emerald Day/Dusk 메타데이터는 +Y up·+Z front·+X coordinate와 UnitScaleFactor=100(미터)이다.

Bistro는 FBX Camera 노드의 원점·+X 전방·+Y 위쪽을 모델과 같은 계층·정규화 변환으로 계산한다. 단위가 다른 legacy Position/Interest 속성은 사용하지 않는다. Camera 또는 조상 노드가 실제로 움직이는 클립을 선택하고, 그 노드 체인의 트랙만 따로 보관한다. 기본 60초(1분)의 현재 투어 시각을 원본 클립 시각으로 환산해 매 프레임 원본 위치·회전·스케일 키를 평가하고 체인의 행렬만 합성한다. 원본 키의 보간 방식·부모 움직임·roll을 유지하며, 원본에 있는 컷에 별도 전환 구간을 추가하지 않는다. FBX 카메라의 FOV 애니메이션은 가져오지 않으며 수직 FOV 50도를 사용한다. 해당 애니메이션이 없으면 원본 bind 카메라를 정지 유지하고, Camera 노드가 없으면 명시적으로 종료한다. 시작 위치·시선·장면 크기를 로그로 출력한다.

Emerald도 원본 FBX Camera 체인을 사용한다. Camera 노드의 scale·PostRotation을 포함한 계층을 유지하며 노드보다 100배 큰 legacy Camera.Position은 사용하지 않는다. 원본 약 64.17초 클립 전체를 60초로 재생하고 마지막 원본 구도를 유지한다. 별도로 추가했던 마지막 15초 조감 동선은 장면 경계·HDRI 배경이 노출되는 문제 때문에 제거했다. 렌즈는 수직 FOV 50도 근사이며 원본 FOV를 정확히 재현했다고 주장하지 않는다.

외부 Bistro에는 원본 노드 이름과 BaseColor 텍스처 이름이 함께 일치하는 나무 잎·담쟁이 잎·꽃 submesh만 바람에 작게 회전시킨다. 같은 노드의 잎 submesh는 정규화한 월드 bounds의 아래쪽 중심을 공통 pivot으로 사용한다. 나무줄기·화분·가구·건물·유리는 고정하며, 지면의 낙엽도 대상으로 삼지 않는다. 이 rigid sway는 스키닝이나 잎별 vertex bending을 재현하지 않는다. Entity transform으로 처리하므로 Main·Surface·Shadow·이전 transform 기반 motion이 같은 자세를 사용한다. 외부에서는 바람이 기본 ON이고 N으로 끈다. 실내는 작은 잎의 움직임 때문에 점광원 전체 그림자 atlas가 매번 갱신되는 것을 피하기 위해 모션을 추가하지 않는다. 광원이나 발광에 무작위 깜빡임을 넣지 않는다.

바람 시계는 카메라의 60초 투어와 분리되어 자유 카메라·투어 종료 후에도 흐른다. Space와 창 비활성화에서는 멈추고 R은 카메라와 바람 시각을 함께 0으로 돌린다. F5/F6 수집은 카메라와 함께 바람 시각·ON/OFF를 저장·복원하고 정지시킨다. 조건 문자열에도 같은 정보를 넣으며 N으로 상태를 바꾸면 기존 비교를 지운다. 기본 바람과 입력 동작만 바꾸고 설정 파일은 생성하지 않는다.

FBX 애니메이션 import는 실제로 애니메이션된 노드만 TRS를 bake하고 상수 키를 정리한 뒤 저장할 키 수를 계산한다. 외부 장면에서 정적 노드까지 전체 bake 대상으로 세어 예산을 초과하던 계산을 수정했으며, 저장 키 상한 10,000,000개는 유지한다. 이 import 단계의 키 예산과 카메라 재생은 별개다. 종전의 투어용 301개 위치·시선·up 중간 샘플과 그 사이의 선형 재보간은 제거했다. 30초 재생에서 약 0.1초마다 근사 경계가 생기던 경로를 없앤 변경이며, 이번 카메라 변경은 아직 실행하지 않아 실제 끊김이나 렌더링 프레임 저하가 해결됐다고 확인하지 않았다.

Sponza 파일은 기존 10개 키 동선을 사용한다. [Sponza.camera.csv](Sponza.camera.csv)는 그 회귀 장면의 참고 자료이며 Bistro 동선이 아니다. 실행 중 CSV를 읽지 않는다. 일반 입력은 원본 Y-up을 전제로 기본 60초 바깥쪽 궤도를 사용한다.

첫 업로드와 실내 probe 준비 시간은 투어에서 제외하고 마지막 구도를 유지한다. 자유 카메라에서는 투어 시계만 멈추며, 창 비활성화·최소화는 바람 시계도 멈춘다. Wayland의 첫 화면은 포커스 없이 제출하며, 창 크기에 맞춰 카메라 종횡비를 갱신한다.

## 빌드와 실행

Wayland용 별도 CMake는 필요하지 않다. Vulkan·Wayland·xkbcommon 개발 파일과 wayland-scanner가 준비된 환경에서 저장소 루트 기준으로 구성한다.

```bash
cmake -S showcase -B build \
  -DUSE_VULKAN=ON -DDY_LINUX_WINDOW_SYSTEM=WAYLAND -DCMAKE_BUILD_TYPE=Release \
  -DDY_SHOWCASE_SCENE=/home/suhan/dev/assets/Bistro_v5_2/BistroExterior.fbx \
  -DDY_SHOWCASE_ENVIRONMENT=/home/suhan/dev/assets/Bistro_v5_2/san_giuseppe_bridge_4k.hdr
cmake --build build --target ShowcaseCinematic --parallel 4
./build/showcase
./build/showcase interior
./build/showcase emerald
```

현재 로컬 빌드 캐시는 build/dependencies에 풀어 둔 Debian 개발 패키지를 사용한다. 이 캐시를 재사용할 때에는 빌드용 pkgconf를 위해 아래 환경 변수를 해당 명령에만 전달한다. 시스템·Sway·foot 설정은 변경하지 않는다.

```bash
LD_LIBRARY_PATH=/home/suhan/dev/dy_engine/build/dependencies/root/usr/lib/x86_64-linux-gnu \
  cmake --build build --target ShowcaseCinematic --parallel 4
```

Windows_DX·Windows_VK·Android·iOS 산출물을 이 Linux 실행 파일로 대체하지 않는다. 해당 플랫폼 빌드와 실제 기기 동작은 별도 작업이다.

## 조작

| 키 | 동작 |
| --- | --- |
| Space / R / Tab | 투어·바람 재생·일시정지 / 두 시계 처음부터 재생 / 투어·자유 카메라 |
| N | 외부 잎·꽃의 바람 ON/OFF. 실내는 모션 대상 없음 |
| RMB / WASD / Q E / Shift | 자유 시선 / 이동 / 하강·상승 / 4배 속도 |
| 1 / 2 / 3 | 조명·기본색 표시 / 그림자 / HDR |
| 4 / 5 / 6 | CPU 컬링 / 인스턴싱 / 병렬 RenderGraph 기록 |
| L / C | 실제 indexed LOD ON/OFF / LOD 색상 표시 ON/OFF |
| F9 | HDR·고급 래스터가 켜진 지원 GPU에서 LOD 색상의 wireframe 표시 ON/OFF |
| [ / ] | LOD 허용 오차 절반 / 두 배. 0.125~16px, 기본 1px. 큰 값은 더 거친 LOD를 허용 |
| 7 / 8 / 9 / 0 | SSAO / SSGI(실내 기본 OFF) / SSR / TAA |
| B / T / G | Bloom / 투명 패스 / 고급 래스터 경로 |
| V | 표면 debug: 기본색 → 법선 → roughness → metallic → depth → motion → 끄기 |
| F1 / F2 / F3 / F4 | 직접광 / 확산 IBL / 반사 IBL / 하늘 |
| - / + | 노출 감소·증가. +는 = 키 |
| M / F11 / Escape | 렌더 제출 수 / 프로파일러 HUD / 종료 |
| F8 | A/B 비교 대상: 정적 그림자 재사용 → 병렬 기록 → 인스턴싱 → indexed LOD |
| F5 / F6 / F7 | 최적화 OFF 기준 수집 / 같은 구도에서 ON 수집 / 수집 취소·결과 지우기 |

키로 바꾼 값은 실행 중에만 적용하며 설정 파일을 쓰지 않는다. Space는 자유 카메라를 해제하지 않는다. M은 새 렌더 제출 횟수이며 화면 표시 완료 FPS가 아니다.

LOD 픽셀 오차는 화면 기준의 품질 조절값이다. [는 더 엄격한 디테일, ]는 더 큰 근사 오차를 허용하며 A/B 비교 중에는 값을 고정한다.

LOD 선택은 OS 이름이 아니라 카메라·물체 변환·내부 렌더 해상도·허용 오차를 사용한다. 같은 조건이면 Android/iOS에서도 같은 기준이며 별도 모바일 LOD 기본값은 아직 없다. 같은 FOV에서 내부 해상도를 낮추면 투영 오차가 작아져 더 거친 단계가 선택될 수 있다. 물리적인 화면 크기는 입력이 아니다. 가까운 물체·축약 단계가 없는 mesh는 LOD0를 유지한다. 텍스처 해상도·조명·후처리 품질은 indexed LOD가 줄이지 않는다.

F9는 opaque/masked geometry를 선택된 LOD의 실제 index로 그리며 solid Surface depth로 가려진 선을 제거한다. Blend 유리는 진단에서 제외한다. Shadow·환경 캡처는 기존 solid 경로를 유지한다. wireframe의 GPU 지원 여부는 RHI에서 조회하며 모바일에서 필수 기능으로 요구하지 않는다.

## Profiling과 최적화 비교

F11은 공통 HUD를 표시한다. FRAME은 성공한 렌더 호출의 완료 간격이며 앱 작업·표시 주기 제한·프레임 확보 실패 사이의 시간도 반영한다. 화면 표시 완료를 계측한 FPS나 입력 지연은 아니다. CPU WORK는 렌더러 내부 prepare + record + submit의 wall time이다. acquire·present·readback은 별도로 표시하며, 앱 시뮬레이션·이벤트 처리는 CPU WORK에 포함하지 않는다. 프레임 확보가 일시적으로 실패했을 때의 acquire 호출 시간도 다음 성공 프레임에 누적한다.

GPU SPAN은 프레임 데이터 업로드 시작부터 RenderGraph 최종 배리어까지의 timestamp 간격이다. Surface·AO·환경광·Main·GI/SSR·투명·후처리·HUD를 포함하며 큐의 빈 시간도 포함할 수 있다. 초기 에셋 업로드·프레임 확보·readback·present는 제외한다. 패스별 시간은 실제 등록된 그래프 패스에서 수집하며 GPU 완료 fence 이후 비동기로 읽는다. 패스 합계와 SPAN을 같은 수치로 취급하지 않는다. 캐시돼 실행하지 않은 Shadow나 미지원·미완료 표본을 0ms로 넣지 않는다. MainForward 시간은 해당 패스만이며 후처리를 포함하지 않는다. Main draw·instance·triangle 및 shadow draw·instance·triangle을 별도로 표시한다.

LOD HUD는 Main의 LOD0~3 인스턴스 분포와 `mainOriginalTriangleCount` → `mainTriangleCount`를 표시한다. 앞 수치는 같은 Main 가시 인스턴스를 원본 LOD0로 제출할 때의 삼각형 수이고 뒤는 선택한 index buffer의 실제 제출량이다. 전체 에셋 삼각형 수·Shadow 제출량·GPU timestamp와는 구분한다. C 색상은 단계 식별, F9 wireframe은 실제 선택한 삼각형의 밀도 확인에 사용한다. Space로 멈춘 뒤 L로 원본과 LOD를 교체해 같은 구도의 차이를 볼 수 있다. wireframe 미지원 기기에서는 C와 F11로 확인한다. 진단 표시에서는 조명·화면 효과·노출·톤매핑을 우회하므로 정상 화질·성능의 F5/F6 비교는 C와 F9를 끄고 수행한다. 원래 공개 품질 설정은 보존한다.

Wine 실내 기본 비교는 정적 point shadow atlas 재사용, Bistro 외부는 병렬 명령 기록, Emerald는 `INDEXED LOD`다. F8로 비교 대상을 고른 뒤 F5로 기준을 수집하고 완료 후 F6으로 ON 결과를 수집한다. F5 때의 구도·투어 시각·바람 시각·바람 ON/OFF를 저장하며 F6에서 복원한다. 카메라와 바람은 고정하고 각 수집은 최소 1초·10제출 프레임 워밍업 후 최소 3초·30제출 프레임을 모은다. 성능이 낮으면 그만큼 더 오래 걸린다. 큰 프로파일러 HUD는 두 수집에서 자동으로 숨기고 같은 작은 진행 표시를 사용한다. 측정은 계속되며 F11로 패스별 내역을 다시 열 수 있다. 결과의 FRAME 막대는 33.33ms를 전체 폭으로 삼는다. 초기 probe 생성 비용은 정상 프레임 A/B에 포함하지 않고 별도 준비 비용으로 설명한다.

F8의 네 번째 항목 `INDEXED LOD`는 현재 로딩한 지원 장면에서 사용할 수 있다. F5는 저장한 같은 시각·구도에서 LOD OFF, F6은 같은 상태로 복귀해 ON으로 수집한다. 픽셀 오차·hysteresis·해상도·나머지 효과는 양쪽에서 유지한다. 삼각형 수 감소는 기하 디테일을 근사한 결과이므로 완전히 같은 geometry를 그리는 중복 작업 제거와 구분한다. 줄어든 삼각형이 실제 GPU 시간 개선으로 이어지는지는 측정 전이다.

창 크기·포커스·카메라·품질 변경은 수집을 취소한다. F6은 선택한 최적화 외의 조건이 다르면 거절한다. 결과는 표본 수로 가중한 평균과 관측 최대 프레임 시간, Main 작업량, GPU 패스 시간을 화면·콘솔로 표시한다. 최대는 p95/1% low가 아니며 파일은 자동 생성하지 않는다. 두 수집 후에도 투어는 정지 상태이고 Space로 재생한다. VSync 상태는 두 수집에서 같아야 하며, 표시 주기 제한으로 FPS가 같더라도 CPU 작업·GPU 패스 시간은 달라질 수 있다. 병렬 기록이나 인스턴싱의 이득을 측정 전에 가정하지 않는다. Bistro의 많은 고유 메시에는 인스턴싱 이득이 작을 수 있다.

밀집 객체 비교용 [Dense Arena](../arena/README.md)는 같은 공통 프로파일러·비교 UI를 사용한다. 현재 CMake 구성에서는 `cmake --build build --target ShowcaseArena --parallel 4`로 만들며 `./build/arena`로 실행한다. 에셋 기본 경로는 CMake의 `DY_DENSE_ARENA_ASSETS=/home/suhan/dev/assets/kenney_tiny-dungeon`이다. F5/F6은 동일한 정지 게임 상태에서 개별 draw와 인스턴싱을 비교하며 게임 시뮬레이션 최적화 결과로 해석하지 않는다.

## 공통 렌더 경로와 한계

실제 메시 Surface prepass에서 oct 법선·roughness·material AO, linear albedo·metalness, motion·현재/이전 깊이를 생성한다. 방향광·그림자, 외부 HDRI 또는 실내 local probe의 확산·반사, SSAO·SSGI·SSR, TAA, bloom·ACES fitted 톤매핑을 연결한다. 내부 해상도는 기본 hdrRenderScale=1.0이다.

효과를 끄면 해당 RenderGraph 패스를 제외한다. AO의 꺼진 입력은 흰색, 환경광·반사의 꺼진 입력은 검은색이다. SSGI·SSR은 독립 패스이며 둘 다 끄면 원래 HDR을 후처리에 전달한다. TAA OFF는 resolve·jitter, bloom OFF는 다운샘플·업샘플을 제외한다. Surface prepass도 표면 정보가 필요한 효과가 없으면 제외한다. T는 투명 합성 패스를 전환한다.

투명 경로는 opaque HDR·depth와 환경 radiance를 사용한 Fresnel 반사·화면 공간 굴절·투과·색 흡수를 대상으로 한다. 실내 유리는 local probe의 방·가구·발광 표면을 반사하고 박스 위치 보정으로 표면 위치에 따라 다른 반사 방향을 얻는다. 직접광의 하이라이트에는 기존 shadow atlas의 PCF 차폐를 연결해 불투명 구조물에 가려진 광원이 그대로 반사되는 경로를 줄인다. 이 차폐는 투명 물체의 광선 투과·색 그림자까지 추적하지 않는다.

모든 유리가 같은 opaque HDR을 샘플하므로 앞쪽 유리가 뒤쪽 유리·액체를 포함해 굴절하지 못하며 transmission=1일 때 앞서 그린 투명 표면을 덮는다. FBX의 point·spot·directional 광원은 장면과 같은 정규화 변환으로 가져오며 하나라도 있으면 임의의 기본 sun을 추가하지 않는다. Renderer는 기존 광원 선택·배열 한도를 사용한다. Wine의 emission에는 배포본 스크립트의 1000배 보정을 적용한다. FBX의 광도 단위·원본 감쇠 방식과 현재 renderer의 일치, area/volume light·Falcor 노출 설정 전체의 재현은 보장하지 않는다.

실내 probe는 한 위치에서 만든 정적 근사다. 박스 투영은 반사 시차를 보정하지만 실제 장면을 추적하거나 방마다 차폐를 계산하지 않는다. diffuse SH도 공간 전체에 대해 하나의 샘플을 사용하므로 벽 너머 빛 누출·구석별 변화·다중 반사를 해결한 lightmap/probe volume/완전 GI가 아니다. 투명 물체는 probe 캡처에 없고 카메라 또는 장면 변경에 따라 매 프레임 새로 반영하지 않는다. 외부 식생의 rigid sway와 실내의 정적 조명 준비를 구분하며 RT는 사용하지 않는다.

SSGI·SSR은 화면 정보에 한정되고 전역 HDRI 확산광은 벽의 하늘 차폐를 직접 계산하지 않는다. 이전 스키닝 포즈·변형 메시의 모션도 미지원이다. V는 실제 메시 Surface 데이터의 기본색·법선·roughness·metallic·depth·motion을 표시한다. debug 표시에서는 resolve를 건너뛰므로 TAA jitter와 시간 변화 샘플 seed도 중지한다. 이를 완전한 deferred 조명 구현과 동일하게 취급하지 않는다.

로딩 동시성은 이 예제에서 이미지 디코드 4개·렌더 텍스처 준비 4개로 설정한다. 공통 기본값은 1이다. 밉 생성은 동시에 4개까지만, GPU 업로드는 이미지 하나씩 처리한다. Surface는 공통 frustum 컬링과 CPU 자료 재사용, 환경 AO는 native bilinear 1회, 사용하지 않는 legacy tone PSO는 생성 생략을 적용했다.

프레임 비용을 줄이기 위해 AO의 기존 RGBA16F 타깃에 대표 깊이·법선 가이드를 함께 저장하고 양방향 필터에서 재사용한다. AO 해상도·탭 수는 유지한다. 고급 투명 패스가 처리할 Blend 표면은 Main의 정렬·일반 descriptor 준비에서 제외하며 Main/Shadow descriptor 캐시는 hash 이진 검색 후 전체 비교로 찾는다. 고정 point/spot-only stock 경로는 성공 제출한 shadow atlas를 재사용한다. 카메라 이동만으로는 무효화하지 않고 caster·광원·alpha 입력·atlas가 바뀌면 다시 그린다. 방향광·혼합 광원·custom 입력은 계속 갱신한다. Wine의 네 정적 점광원에는 24개 view 재사용이 적용되며 exterior의 태양 CSM에는 적용되지 않는다. 공통 LightingConfig::reuseStaticShadows의 기본값은 true이며 비교 때만 해당 값을 바꾼다. 이들 중복 작업 제거는 기존 품질 설정을 유지한 변경이다. 별도로 이번 실내 조명 구성은 local probe를 도입하고 SSGI를 기본 OFF로 바꿨으므로 이전 설정과 같은 렌더 구성이 아니다. 프로파일러와 A/B 수집을 포함한 Wayland/Vulkan 실행 파일을 빌드했으며 실제 개선 전후 성능과 실행 화면은 이번 작업에서 확인하지 않았다.

source·parser input·parser heap·decoded 예산은 각각 8GiB다. 프로세스 전체 RAM·VRAM 상한이 아니며 DDS 압축 파일 크기와 디코딩 메모리도 다르다. 원본 이미지 해상도는 유지한다.

glTF의 반복 primitive는 node-local geometry를 한 번 디코딩하고 노드별 transform·재질을 분리한다. 공개 `ModelData`의 소유 항목은 `ModelMesh::geometryIndex=UINT32_MAX`이며 반복 항목은 빈 geometry와 이전 소유 항목의 인덱스를 가진다. 직접 소비자는 vertex/index를 해당 원본에서 읽고 node·skin·morph weight·재질 정보는 현재 항목에서 읽어야 한다. 예전처럼 모든 static 항목의 vertex가 world-baked라고 가정하면 안 된다. `LoadModelAsset`은 같은 immutable geometry에 하나의 MeshID를 사용하며 bounds·`MergeModelMeshes`도 노드 transform과 참조를 반영한다. skin·morph geometry는 기존 독립 경로, 애니메이션 없는 mirror transform은 기존 winding을 보존하는 baked 경로로 남긴다. `EXT_mesh_gpu_instancing`은 아직 지원하지 않으며 일반 glTF 노드가 같은 mesh를 참조하는 배치를 대상으로 한다.

FBX의 반복 geometry도 원본 mesh identity·geometry-to-node offset의 정확한 성분·실제 node material slot 배치가 모두 같으면 같은 canonical geometry를 참조한다. 각 node의 world transform은 유지하며 offset·재질 override가 다르면 독립 디코딩한다. skin·morph는 독립 경로, 정적 mirror는 기존 world-baked 경로를 유지한다. 디코딩 예산은 공유 geometry 한 번과 배치별 메타데이터를 계산하고 광원 bounds는 각 인스턴스의 world 위치를 포함한다. 서로 다른 원본 mesh의 내용을 비교해 합치는 기능은 아니다.

indexed LOD는 원본 vertex buffer를 공유하고 최대 3개의 축약 index buffer를 추가한다. `BuildMeshLods`는 immutable mesh별 최초 준비 때 한 번 CPU에서 생성하고 캐시에 보관한다. 공통 기본값은 `enableLod=true`, `lodPixelError=1.0`, `lodHysteresis=0.15`, `debugLodColor=false`다. 투영한 오차를 픽셀 기준으로 비교하고 전환 구간의 hysteresis로 경계에서 단계가 반복 교체되는 것을 줄인다. Main·Surface·투명 패스는 같은 선택을 사용하고 Shadow는 LOD0를 유지한다. 인덱스만 바꾸므로 원본 vertex 저장·전체 텍스처 비용은 사라지지 않으며 추가 index buffer의 CPU/GPU 메모리와 최초 생성 시간이 필요하다. 경계·UV/법선 seam을 엄격히 보호하므로 어떤 mesh는 단계를 줄이거나 추가 LOD를 만들지 못한다. 모든 mesh가 LOD3까지 줄어든다고 가정하지 않는다.

FBX 로더가 삼각형 corner별로 저장한 정점은 LOD 생성 안에서 동일한 위치·법선·UV·접선·색상끼리 임시로 재인덱싱한다. 연결된 표면이 모두 seam으로 오인돼 축약되지 않던 입력 문제를 수정한 것이다. 축약한 인덱스는 원본 정점의 대표 인덱스로 되돌려 GPU의 원본 VB를 그대로 사용하며 실제 속성 경계는 유지한다. 오차값은 위치와 법선/UV를 함께 평가한 가중 근사값을 물체 크기로 환산한 것으로, 순수 기하 거리의 엄밀한 상한은 아니다. 실제 생성 단계 수·LOD 분포·삼각형 감소는 사용자 실행 확인 대상이다.

RHI indirect와 독립 [17 예제](../../examples/advanced/17_IndirectDrawing)는 구현되어 있지만 이 환경 경로의 GPU culling·indirect 소비는 미연결이다. 공통 renderer의 indexed LOD와 독립 [23 예제](../../examples/advanced/23_MeshletCullingLOD)의 미완성 meshlet 경로는 서로 다르다. 대량 적/탄환/소품은 별도 Dense Arena의 CPU 컬링·instancing 경로이며 광역 에셋 로딩·실제 장면 통합과 모바일 앱 연결은 [후속 기획](../Presentation.md)에 남아 있다. 구현된 렌더 경로와 해당 에셋에서의 실행 완료를 구분한다.

사용자 확인 항목은 Bistro DDS·재질/유리 표현, 실내 probe 반사·박스 경계·그림자 하이라이트 차폐, 외부 식생과 그림자/motion, Emerald 60초 원본 카메라의 끝 구도·자동 노출·재질/조명, 입력·resize·각 패스, 정지/이동 TAA 흔들림·잔상·잔여 흰 점, 공유 geometry의 노드 배치, LOD 전환·실루엣·alpha·wireframe·단계별 통계, 초기 probe/LOD 준비 시간·메모리·정상 프레임 비용이다. 이번 변경은 실행 테스트하지 않았고 실제 화질·FPS는 미측정이다. Jungle 원본의 엔진 실행도 미완료다. 원본 Falcor·Intel 결과와 동등한 화질 또는 특정 성능을 확인했다고 주장하지 않는다.
