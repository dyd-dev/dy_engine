# Cinematic Showcase 에셋 크레딧

## Amazon Lumberyard Bistro

- 저작자·자료명: **Amazon Lumberyard Bistro, Open Research Content Archive (ORCA)**, 2017.
- 배포처와 다운로드: [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro), [공식 다운로드](https://developer.nvidia.com/bistro).
- 이용 조건: **Creative Commons Attribution 4.0 International**. 원문은 `/home/suhan/dev/assets/Bistro_v5_2/LICENSE.txt`에 보존한다.
- 원본: `/home/suhan/dev/assets/Bistro_v5_2.zip`.
- 추출 폴더: `/home/suhan/dev/assets/Bistro_v5_2/`.
- 장면: `BistroExterior.fbx`, `BistroInterior.fbx`, `BistroInterior_Wine.fbx`.
- 포함 하늘: `san_giuseppe_bridge_4k.hdr`. 배포본 README의 원출처는 HDRI Haven으로 표기되어 있다.

원본 FBX·DDS·HDR·pyscene·README·라이선스는 수정하지 않는다. 실행 중 장면 중심·크기·Y-up을 Z-up으로 맞추며, 원본 FBX 카메라의 시작 구도와 노드 애니메이션을 사용한다. 카메라 클립은 300초로 재생 시간을 조정한다.

DDS는 원래 해상도의 RGBA8로 디코딩하고 밉맵을 생성한다. 배포본의 Specular 텍스처는 R=AO, G=roughness, B=metallic이며 BaseColor alpha는 opacity다. DirectX 노멀 방향과 BC5의 저장 규약을 반영한다. 현재 GPU의 원본 BC 압축 저장을 보존하는 경로는 아니다.

Wine 장면의 `.pyscene`에 명시된 transmission·IOR·흡수 계수를 형식이 정해진 재질 설정으로 옮긴다. Python 파일을 실행하지 않는다. 유리·액체 두께는 래스터 굴절을 위한 근사값으로 지정한다. 불투명 화면을 사용하는 굴절과 HDRI 반사는 중첩 유리·액체의 완전한 광학 시뮬레이션을 재현하지 않는다. 예제의 조명·후처리 결과를 원저작자의 렌더링 결과로 표시하지 않는다.

원본 Falcor용 `.pyscene`의 환경광 intensity 10과 Wine 발광 1000배는 배포본의 렌더 설정이다. 이를 현재 renderer의 exposure 1에 그대로 적용해 과노출이 발생했으므로 예제의 두 추가 배율을 제거했다. 현재 환경광은 Renderer 기본 intensity 1, 발광은 FBX에서 가져온 재질 emission을 사용한다. 원본 에셋 파일은 바꾸지 않는다.

영상 크레딧과 설명에는 위 저작자·자료명, ORCA 링크, CC BY 4.0 및 변환·렌더링 수정 내역을 남긴다.

## Intel Jungle Ruins — 후속 장면 원본

- 배포처: [Intel GPU Research Samples](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html).
- [공식 다운로드](https://cdrdv2.intel.com/v1/dl/getContent/844047).
- 로컬 압축 파일: `/home/suhan/dev/assets/JungleRuins_1_0_1b.zip`.
- 공식 배포 목록의 이용 조건: Creative Commons Attribution. 저작자와 적용 조건의 전체 표기는 번들의 `JungleRuins/credits_license.txt`를 따른다.

이 원본은 후속 식생·LOD 장면용으로 내려받았으며 현재 실행 파일에는 연결하지 않았다. Blender·USD 입력 변환과 원본의 타일·인스턴스 보존이 필요하다.

## Intel New Sponza Base Scene

개발 중 효과 비교에 사용한 환경 장면은 Intel Sponza 2022 기본 장면이다.

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
