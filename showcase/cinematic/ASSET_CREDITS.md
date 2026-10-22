# Cinematic Showcase 에셋 크레딧

## Amazon Lumberyard Bistro

- 저작자·자료명: **Amazon Lumberyard Bistro, Open Research Content Archive (ORCA)**, 2017.
- 배포처와 다운로드: [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro), [공식 다운로드](https://developer.nvidia.com/bistro).
- 이용 조건: **Creative Commons Attribution 4.0 International**. 원문은 `/home/suhan/dev/assets/Bistro_v5_2/LICENSE.txt`에 보존한다.
- 원본: `/home/suhan/dev/assets/Bistro_v5_2.zip`.
- 추출 폴더: `/home/suhan/dev/assets/Bistro_v5_2/`.
- 장면: `BistroExterior.fbx`, `BistroInterior.fbx`, `BistroInterior_Wine.fbx`.
- 포함 하늘: `san_giuseppe_bridge_4k.hdr`. 배포본 README의 원출처는 HDRI Haven으로 표기되어 있다.

원본 FBX·DDS·HDR·pyscene·README·라이선스는 수정하지 않는다. 실행 중 장면 중심·크기·Y-up을 Z-up으로 맞추며, 원본 FBX 카메라의 시작 구도와 노드 애니메이션을 사용한다. 기본 카메라 투어는 60초로 재생 시간을 조정하며, 별도 카메라 CSV를 지정하면 해당 경로의 재생 시간을 따른다.

DDS는 원래 해상도의 CPU RGBA8로 디코딩한다. Vulkan에서 지원을 확인한 BC1·BC3의 완전한 원본 mip chain은 압축 상태로 GPU에 업로드한다. BC5 노멀의 재구성·스위즐 입력, 불완전 mip 및 다른 백엔드는 기존 RGBA8·CPU mip 생성 경로를 유지한다. 배포본 README는 Specular R/G/B를 occlusion/roughness/metalness로 설명하지만, [Falcor 형식 문서](https://raw.githubusercontent.com/NVIDIAGameWorks/Falcor/master/docs/usage/scene-formats.md)는 R occlusion을 unsupported로 명시한다. ORCA 프리셋은 G/B를 사용하고 R을 AO로 적용하지 않는다. BaseColor alpha는 opacity이며 명시적인 Mask 규약을 선택한다. DirectX 노멀 방향과 BC5의 저장 규약을 반영한다. 원본 mip 사용 결과가 기존 재생성 mip과 RGB까지 같다고 주장하지 않는다.

Wine 장면의 `.pyscene`에 명시된 transmission·IOR·흡수 계수를 형식이 정해진 재질 설정으로 옮긴다. Python 파일을 실행하지 않는다. 유리·액체 두께는 래스터 굴절을 위한 근사값으로 지정한다. 불투명 화면을 사용하는 굴절과 HDRI 반사는 중첩 유리·액체의 완전한 광학 시뮬레이션을 재현하지 않는다. 예제의 조명·후처리 결과를 원저작자의 렌더링 결과로 표시하지 않는다.

현재 낮 외부는 원본 Falcor `.pyscene`의 환경광 intensity 10과 exposure 0.04를 사용한다. Wine 실내는 배포본의 발광 1000배와 exposure 0.12를 사용하고 기본 Interior에서 점광원 4개를 복원한다. 원본 에셋 파일은 바꾸지 않는다.

`night` 또는 `exterior --night`는 엔진 시연용으로 구성한 별도 야간 조명이다. 외부 FBX에는 태양 1개와 발광 재질만 있으므로, 원본 발광 면의 연결 성분과 위치를 기준으로 전구·가로등·간판의 analytic light 및 LTC 사각·원형 면광원을 추가한다. 태양은 약한 청색 달빛으로 바꾸고, 낮 HDRI는 프로그램에서 만든 어두운 청색 파노라마로 대체한다. 발광 배율 40·exposure 0.45와 푸른 간접광은 시연용 조정이다. 이것을 원저작자의 야간 렌더나 원본 광원 복원으로 표시하지 않는다. 면광원 그림자는 지원하지 않으며 일부 가로등만 그림자를 생성한다. 야간 조명을 반영한 프로브를 시작할 때 다시 캡처한다.

영상 크레딧과 설명에는 위 저작자·자료명, ORCA 링크, CC BY 4.0 및 변환·렌더링 수정 내역을 남긴다.

## NVIDIA Emerald Square

- 자료명: **NVIDIA Emerald Square, Open Research Content Archive (ORCA)**, 2017년 7월.
- 저작자·원문 인용 표기: **Nicholas Hull, Kate Anderson, Nir Benty**. 번들 README는 Nicholas Hull의 원본과 Kate Anderson의 수정을 명시하며, 식생은 SpeedTree가 제공했다.
- 배포처: [NVIDIA ORCA](https://developer.nvidia.com/orca/nvidia-emerald-square), [공식 다운로드](https://developer.nvidia.com/emerald-square).
- 이용 조건: **Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported**. 번들의 `LICENSE.txt`와 공식 배포 페이지를 따른다.
- 원본: `/home/suhan/dev/assets/EmeraldSquare_v4_1.zip`, 추출 폴더 `/home/suhan/dev/assets/EmeraldSquare_v4_1/`.
- Day 장면·환경: `EmeraldSquare_Day.fbx`, `symmetrical_garden_4k.hdr`. README는 Day/Dusk HDR의 출처를 HDRI Haven으로 표기한다.
- README 삼각형 수: unique 2,695,054개, instance 포함 10,046,405개.

원본 FBX·DDS·HDR·pyscene·README·라이선스를 수정하지 않는다. `emerald` 프리셋은
Y-up→Z-up·크기 정규화, ORCA의 DirectX normal·roughness/metalness 채널과 명시적 Mask를 사용한다.
Specular R을 AO로 읽던 해석과 중간 alpha만으로 Blend를 선택하던 동작을 바로잡았다.
원본 카메라의 수직 FOV 약 33.9666°와 FBX 노드의 `CullingOff` 양면 설정을 보존한다.
이 프리셋의 규약은 일반 파일 경로에 자동 적용하지 않으며, 다른 경로에는 `--scene-preset emerald`로 선택한다.

수정 전후 화면 비교는 장면 15초·기존 FOV 50°·같은 조명·노출·후처리 조건을 사용했다.
원본 FOV를 복원한 최종 성능 측정과 구분한다. 비교 근거는 [렌더링 감사](../RenderingAudit.md)에 기록한다.
Dusk는 발광면으로 조명하는 별도 원본이며 현재 시연 경로에는 연결하지 않았다.

```bibtex
@misc{ORCANVIDIAEmeraldSquare,
  title = {NVIDIA Emerald Square, Open Research Content Archive (ORCA)},
  author = {Nicholas Hull and Kate Anderson and Nir Benty},
  year = {2017},
  month = {July},
  url = {https://developer.nvidia.com/orca/nvidia-emerald-square}
}
```

## Intel Jungle Ruins — 후속 장면 원본

- 배포처: [Intel GPU Research Samples](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html).
- [공식 다운로드](https://cdrdv2.intel.com/v1/dl/getContent/844047).
- 로컬 압축 파일: `/home/suhan/dev/assets/JungleRuins_1_0_1b.zip`.
- 공식 배포 목록의 이용 조건: Creative Commons Attribution. 저작자와 적용 조건의 전체 표기는 번들의 `JungleRuins/credits_license.txt`를 따른다.

이 원본은 후속 식생·LOD 장면용으로 내려받았으며 현재 실행 파일에는 연결하지 않았다. Blender·USD 입력 변환과 원본의 타일·인스턴스 보존이 필요하다.

## Intel New Sponza Base Scene

개발 중 효과 비교에 사용한 환경 장면은 Intel Sponza 2022 기본 장면이다.
현재 Showcase의 필수 에셋과 자동 다운로드 대상에는 포함하지 않는다.

- 저작자 표기: Frank Meinl and Anton Kaplanyan.
- 연도와 자료명: 2022, Intel Sample Library.
- [공식 배포 페이지](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)
- [공식 기본 장면 다운로드](https://cdrdv2.intel.com/v1/dl/getContent/830833)
- 원본 압축 파일: `/home/suhan/dev/assets/Sponza.zip`.
- 실행용 장면: `/home/suhan/dev/assets/main_sponza/NewSponza_Main_glTF_003.gltf`.
- 원문 크레딧과 라이선스: `/home/suhan/dev/assets/main_sponza/credits_license.txt`.

원본의 glTF, 바이너리, 텍스처와 라이선스를 추출해 사용한다. 원본 메시나 텍스처는 수정하지 않는다. 예제는 실행 중 장면을 중앙으로 옮기고 크기와 위쪽 축을 맞춘 뒤 자체 조명과 카메라 경로로 렌더링한다. Add-on 패키지는 포함하지 않는다.

동봉 `credits_license.txt`에는 개인·교육 사용과 제한된 마케팅·인쇄 상업 사용에 관한 안내 및 Creative Commons Attribution 4.0 전문이 함께 들어 있다. 배포 파일의 해당 원문 전체를 유지한다. 영상이나 장면을 공개할 때에도 원본 출처와 저작자, 적용 조건 및 위 변경 내용을 함께 표기한다.

원본이 요청하는 인용 형식은 다음과 같다.

```bibtex
@misc{sponza22,
  Author = {Frank Meinl and Anton Kaplanyan},
  Year = {2022},
  Note = {https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-processing-research/samples.html},
  Title = {Intel Sample Library}
}
```

## Kloppenheim 06 PureSky HDRI

- 자료명: **Kloppenheim 06 PureSky**.
- 제작자: **Greg Zaal, Jarod Guest**.
- 배포처와 원본: [Poly Haven](https://polyhaven.com/a/kloppenheim_06_puresky).
- 이용 조건: [Poly Haven CC0](https://polyhaven.com/license).
- 로컬 파일: `/home/suhan/dev/assets/kloppenheim_06_puresky_4k.hdr`.

이 HDRI는 개발 중 Sponza의 하늘 배경, 확산 환경 조명과 거칠기별 환경 반사에 사용했다. 현재 Bistro 기본 HDRI는 Bistro 번들의 `san_giuseppe_bridge_4k.hdr`이며 기존 CMake 캐시의 선택은 유지한다. Renderer가 실행 중 조명용 계수와 반사 맵을 생성하며 원본 `.hdr` 파일은 수정하지 않는다. 이 HDRI를 사용한 영상에는 위 제작자와 링크를 남긴다.
