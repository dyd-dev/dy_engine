# Arena scene assets

3D 캐릭터와 소품의 제작자는 **Kay Lousberg (KayKit)**다.

- 제작자: <https://kaylousberg.com/>
- 공개 배포 조직: <https://github.com/KayKit-Game-Assets>
- 이용 조건: 원본 배포본의 **CC0 1.0 Universal**.
- 파일과 원문 라이선스는 저장소와 나란한 `../assets/KayKit_Arena/`에 보관한다.
  현재 경로는 `/home/suhan/dev/assets/KayKit_Arena/`다.
- 원문 라이선스 파일명은 `Adventurers-LICENSE.txt`,
  `Skeletons-LICENSE.txt`, `Dungeon-LICENSE.txt`다.

## 가져온 원본

| 배포본 | 공식 저장소 | 사용 파일 |
| --- | --- | --- |
| Adventurers | [KayKit-Character-Pack-Adventures-1.0](https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0) | `Knight.glb`, `knight_texture.png` |
| Skeletons | [KayKit-Character-Pack-Skeletons-1.0](https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Skeletons-1.0) | `Skeleton_Minion.glb`, `Skeleton_Warrior.glb`, `skeleton_texture.png` |
| Dungeon Remastered | [KayKit-Dungeon-Remastered-1.0](https://github.com/KayKit-Game-Assets/KayKit-Dungeon-Remastered-1.0) | `barrel_large.gltf.glb`, `column.gltf.glb`, `chest.glb`, `dungeon_texture.png` |

2026-10-20에 공식 저장소의 `main` 배포 파일을 가져왔다.
Adventurers 원본 경로는 `addons/kaykit_character_pack_adventures/Characters/gltf/`,
Skeletons는 `addons/kaykit_character_pack_skeletons/Characters/gltf/`,
Dungeon은 `addons/kaykit_dungeon_remastered/Assets/gltf/`다.
던전 atlas 원본은 `addons/kaykit_dungeon_remastered/Assets/texture/dungeon_texture.png`다.

GLB 원본은 변경하지 않았다. 장면에서 Z-up·크기·위치를 맞추고,
기사의 여러 장비 중 검 하나와 방패 하나만 표시한다. 이동·공격·주문에 원본
애니메이션을 사용하며, 배치·카메라·조명·렌더 결과는 이 예제의 구성이다.
각 GLB에는 atlas 이미지가 내장돼 있다. 별도 PNG는 원본 자료로 함께 보존한다.

석재·성벽·제단·포탈·룬·폐허와 화살·광탄·불꽃은 이 예제의 C++ 메시로 구성한다.
폰트는 실행 장치에 이미 있는 글꼴을 사용하며 폰트 파일은 포함하지 않는다.
AdvancedRendering의 LTC 표 이용 조건은 기존 엔진의 원문을 실행 파일 옆
`AreaLightingLtc.LICENSE`로 함께 복사한다.
