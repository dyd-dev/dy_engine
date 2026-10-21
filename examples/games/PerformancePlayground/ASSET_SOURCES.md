# 외부 에셋 출처

2026-10-16에 아래 공식 배포 페이지와 패키지 내부 License.txt를 확인했다. 저작자는 모두 **Kenney / Kenney.nl**이며 모두 **CC0 1.0 Universal**이다. 데이터 에셋만 받았으며 구매·가입·외부 업로드는 없다.

| 패키지 | 공식 페이지 | 원본 다운로드 | 용도 |
|---|---|---|---|
| City Kit (Commercial) 2.1 | https://kenney.nl/assets/city-kit-commercial | https://kenney.nl/media/pages/assets/city-kit-commercial/a742d900eb-1753115042/kenney_city-kit-commercial_2.1.zip | 도시 배경, 건물, 차양/처마/파라솔 |
| City Kit (Industrial) 2.0 | https://kenney.nl/assets/city-kit-industrial | https://kenney.nl/media/pages/assets/city-kit-industrial/0ec35b139d-1788171848/kenney_city-kit-industrial_2.0.zip | 산업 건물, 탱크, 굴뚝, 컨테이너, 태양광/풍력, 물탑 |
| Building Kit 1.0 | https://kenney.nl/assets/building-kit | https://kenney.nl/media/pages/assets/building-kit/0de7aaa492-1743244741/kenney_building-kit.zip | 출입 가능한 모듈 실내, 벽/문/창, 바닥/지붕, 기둥/파이프/장식 |
| Animated Characters Protagonists (ZIP License.txt: 1.1, 페이지 표기: 1.0) | https://kenney.nl/assets/animated-characters-protagonists | https://kenney.nl/media/pages/assets/animated-characters-protagonists/608191acc4-1774773108/kenney_animated-characters-protagonists.zip | 3D 플레이어 캐릭터와 스킨, idle/run 원본 |

라이선스 원문: https://creativecommons.org/publicdomain/zero/1.0/legalcode
요약: https://creativecommons.org/publicdomain/zero/1.0/
저작권 범위에서 수정·상업 사용·재배포가 가능하다. CC0는 상표권과 제3자 권리의 보장을 의미하지 않는다. 원저작자의 보증이나 후원을 주장하지 않는다.

`Assets/Kenney/<pack>/License.txt`에 원본 라이선스 파일을 그대로 보관한다. `Assets/manifest.json`에는 배포에 포함한 모든 파일, 원래 ZIP 내부 경로, 크기, SHA256을 기록한다. `FetchAssets.ps1`에는 공식 ZIP URL, 원본 ZIP의 고정 SHA256, 선택 파일 목록이 있다. GLB/FBX/PNG는 원본 데이터이며 월드 배치와 게임 로직은 이 예제에서 작성한다.
