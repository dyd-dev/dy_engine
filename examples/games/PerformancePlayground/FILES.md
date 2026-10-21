# 현재 파일 목록

2026-10-21 main API 호환성 커밋 기준. 게임 폴더 신규 추적 64개, 공통 HLSL 수정 2개. 삭제·물리적 이동은 없다.

## 게임 폴더

- [ASSET_SOURCES.md](ASSET_SOURCES.md)
- [Assets/.gitattributes](Assets/.gitattributes)
- [Assets/Kenney/building/border.glb](Assets/Kenney/building/border.glb)
- [Assets/Kenney/building/column.glb](Assets/Kenney/building/column.glb)
- [Assets/Kenney/building/detail-pipe.glb](Assets/Kenney/building/detail-pipe.glb)
- [Assets/Kenney/building/floor.glb](Assets/Kenney/building/floor.glb)
- [Assets/Kenney/building/gutter-vertical.glb](Assets/Kenney/building/gutter-vertical.glb)
- [Assets/Kenney/building/License.txt](Assets/Kenney/building/License.txt)
- [Assets/Kenney/building/plating-detailed.glb](Assets/Kenney/building/plating-detailed.glb)
- [Assets/Kenney/building/roof-flat-center.glb](Assets/Kenney/building/roof-flat-center.glb)
- [Assets/Kenney/building/Textures/colormap.png](Assets/Kenney/building/Textures/colormap.png)
- [Assets/Kenney/building/wall-doorway-wide-square.glb](Assets/Kenney/building/wall-doorway-wide-square.glb)
- [Assets/Kenney/building/wall-window-square.glb](Assets/Kenney/building/wall-window-square.glb)
- [Assets/Kenney/building/wall.glb](Assets/Kenney/building/wall.glb)
- [Assets/Kenney/characters/Animations/idle.fbx](Assets/Kenney/characters/Animations/idle.fbx)
- [Assets/Kenney/characters/Animations/run.fbx](Assets/Kenney/characters/Animations/run.fbx)
- [Assets/Kenney/characters/License.txt](Assets/Kenney/characters/License.txt)
- [Assets/Kenney/characters/Model/characterMedium.fbx](Assets/Kenney/characters/Model/characterMedium.fbx)
- [Assets/Kenney/characters/Skins/skaterMaleA.png](Assets/Kenney/characters/Skins/skaterMaleA.png)
- [Assets/Kenney/commercial/building-a.glb](Assets/Kenney/commercial/building-a.glb)
- [Assets/Kenney/commercial/building-b.glb](Assets/Kenney/commercial/building-b.glb)
- [Assets/Kenney/commercial/building-c.glb](Assets/Kenney/commercial/building-c.glb)
- [Assets/Kenney/commercial/building-d.glb](Assets/Kenney/commercial/building-d.glb)
- [Assets/Kenney/commercial/building-f.glb](Assets/Kenney/commercial/building-f.glb)
- [Assets/Kenney/commercial/building-k.glb](Assets/Kenney/commercial/building-k.glb)
- [Assets/Kenney/commercial/building-skyscraper-a.glb](Assets/Kenney/commercial/building-skyscraper-a.glb)
- [Assets/Kenney/commercial/detail-awning-wide.glb](Assets/Kenney/commercial/detail-awning-wide.glb)
- [Assets/Kenney/commercial/detail-awning.glb](Assets/Kenney/commercial/detail-awning.glb)
- [Assets/Kenney/commercial/detail-overhang.glb](Assets/Kenney/commercial/detail-overhang.glb)
- [Assets/Kenney/commercial/detail-parasol-a.glb](Assets/Kenney/commercial/detail-parasol-a.glb)
- [Assets/Kenney/commercial/License.txt](Assets/Kenney/commercial/License.txt)
- [Assets/Kenney/commercial/Textures/colormap.png](Assets/Kenney/commercial/Textures/colormap.png)
- [Assets/Kenney/industrial/building-a.glb](Assets/Kenney/industrial/building-a.glb)
- [Assets/Kenney/industrial/building-b.glb](Assets/Kenney/industrial/building-b.glb)
- [Assets/Kenney/industrial/building-c.glb](Assets/Kenney/industrial/building-c.glb)
- [Assets/Kenney/industrial/chimney-medium.glb](Assets/Kenney/industrial/chimney-medium.glb)
- [Assets/Kenney/industrial/chimney-small.glb](Assets/Kenney/industrial/chimney-small.glb)
- [Assets/Kenney/industrial/detail-tank.glb](Assets/Kenney/industrial/detail-tank.glb)
- [Assets/Kenney/industrial/License.txt](Assets/Kenney/industrial/License.txt)
- [Assets/Kenney/industrial/shipping-container-a.glb](Assets/Kenney/industrial/shipping-container-a.glb)
- [Assets/Kenney/industrial/solar-panel-landscape.glb](Assets/Kenney/industrial/solar-panel-landscape.glb)
- [Assets/Kenney/industrial/Textures/colormap.png](Assets/Kenney/industrial/Textures/colormap.png)
- [Assets/Kenney/industrial/water-tower.glb](Assets/Kenney/industrial/water-tower.glb)
- [Assets/Kenney/industrial/windmill-low.glb](Assets/Kenney/industrial/windmill-low.glb)
- [Assets/manifest.json](Assets/manifest.json)
- [Build.ps1](Build.ps1)
- [CMakeLists.txt](CMakeLists.txt)
- [CompatibilityTests.cpp](CompatibilityTests.cpp)
- [Controller.h](Controller.h)
- [ControllerTests.cpp](ControllerTests.cpp)
- [Controls.h](Controls.h)
- [Evidence.h](Evidence.h)
- [FetchAssets.ps1](FetchAssets.ps1)
- [FILES.md](FILES.md)
- [MAIN_API_VERIFICATION.md](MAIN_API_VERIFICATION.md)
- [main.cpp](main.cpp)
- [PLAN.md](PLAN.md)
- [README.md](README.md)
- [TRACY_VERIFICATION.md](TRACY_VERIFICATION.md)
- [TracyMemory.cpp](TracyMemory.cpp)
- [TracyMemoryTests.cpp](TracyMemoryTests.cpp)
- [VERIFICATION.md](VERIFICATION.md)
- [World.cpp](World.cpp)
- [World.h](World.h)

## 공통 엔진 수정

- [src/dyf/Shaders/mesh_ps.hlsl](../../../src/dyf/Shaders/mesh_ps.hlsl): HLSL 예약어와 충돌하는 지역변수 이름 변경.
- [src/dyf/Shaders/mesh_shadow_ps.hlsl](../../../src/dyf/Shaders/mesh_shadow_ps.hlsl): 그림자 VS/PS 입력 레지스터 정렬.

## 커밋에서 제외한 로컬 결과물

- build/performance-playground-main/: 기본 Release/Debug 빌드와 테스트 결과.
- build/performance-playground-main-tracy/: Tracy ON 빌드와 테스트 결과.
- build/performance-playground-commit-prep-20261021/: 이전 상태 보존 명세, 파일 목록, 실패 재현/검증 로그, 구역별 JSON, 컨트롤러 독립 빌드.
- 이전 build/performance-playground-native/ 및 기존 캡처는 그대로 보존한다.

42개 원본 에셋의 경로·크기·SHA256은 [Assets/manifest.json](Assets/manifest.json)에 있다. 현재 검증 결과는 [MAIN_API_VERIFICATION.md](MAIN_API_VERIFICATION.md)에 있다.
