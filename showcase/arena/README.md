# Emberkeep — The Sunken Courtyard

발표용 3D 던전 장면이다. 성채 광장에 기사·해골·소품을 배치하고 조명,
골격 애니메이션, 반복 geometry, 투사체 연출을 함께 보여준다.
플레이어 조작·체력·점수·웨이브 같은 게임 규칙은 없다.

## 장면 구성

- 베벨 석재 바닥, 성벽·아치문·탑·폐허, 중앙 수정 제단과 룬 문양.
- 청록색 제단·포탈과 호박색 화로, 달빛·그림자·SSAO·안개·bloom.
- KayKit 기사 1개와 해골 72개가 기본값이며, 해골 수는 12~512개로 지정할 수 있다.
- 원본 이동·공격·주문 애니메이션과 시간에 따라 반복되는 광탄·충격 불꽃.
- 캐릭터·소품은 공유 원본을 사용하고, 석재와 이펙트는 메시·재질별 instance를 사용한다.
- 배치와 광탄 연출은 절대 시각으로 정한다. 정지하면 카메라와 애니메이션도 멈춘다.

## 빌드와 실행

저장소 루트 빌드에서 `DY_BUILD_SHOWCASE=ON`으로 선택하면 필요한
Model·AdvancedRendering과 `ShowcaseArena`가 활성화된다. 실행 파일 이름은 `arena`다.
`DY_BUILD_EXAMPLES=ON`인 경우 기존 visualizer에도 같은 예제 이름으로 나타난다.

저장소 루트에서 Wayland용 Arena만 선택하는 명령은 다음과 같다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDY_LINUX_WINDOW_SYSTEM=WAYLAND -DDY_BUILD_EXAMPLES=OFF -DDY_BUILD_SHOWCASE=ON -DDY_SHOWCASE_ARENA_ONLY=ON
cmake --build build --target ShowcaseArena -j
./build/showcase/arena
```

셰이더 컴파일러와 플랫폼 개발 도구는 해당 빌드 머신에 준비되어 있어야 한다.

```text
arena [--assets PATH] [--actors 72] [--paused] [--font PATH] [--frames N] [--profiler] [--validation] [--touch]
```

에셋 기본 경로는 저장소와 나란한 `../assets/KayKit_Arena/`다.
현재 환경에서는 `/home/suhan/dev/assets/KayKit_Arena/`다.
상위 showcase의 `DY_ASSET_ROOT`가 있으면 그 아래 `KayKit_Arena/`를 기본값으로 사용한다.
CMake의 `DY_ARENA_GAME_ASSETS` 또는 실행 인자 `--assets`로 다른 폴더를 지정할 수 있다.
기존 Tiny Dungeon 경로는 사용하지 않는다.
Arena를 구성하면 누락된 공식 KayKit 파일과 라이선스를 지정한 에셋 폴더에
자동으로 받는다. 기존 파일은 재사용한다.
전체 Showcase 다운로드 동작은 [상위 안내](../README.md)를 따른다.
`--objects`는 `--actors`와 같은 인자이며 범위도 12~512개다.
`--frames`는 성공한 제출 횟수로 종료하며 0이면 계속 표시한다.
글자는 기존 시스템 글꼴을 사용하고, 지원 글꼴이 없으면 내장 비트맵으로 표시한다.
`--profiler`는 처음부터 펼친 Profiler를 표시한다. 공통 Profiler의 공개 측정
스냅샷을 Arena의 글꼴·레이아웃으로 표시하며 엔진의 측정 코드는 변경하지 않는다.
`--validation`은 해당 실행에 Vulkan 검증을 켜며 기본값은 OFF다.
`--touch`는 데스크톱에서도 포인터·터치 버튼을 표시한다. Android·iOS 빌드는
기본으로 버튼을 표시한다. 모바일 빌드 선택은 [상위 안내](../README.md)를 따른다.
데스크톱 실행 파일만 다른 머신으로 옮겼다면 `--assets`로 해당 머신의 에셋 폴더를 지정한다.

## 장면 조작

| 입력 | 동작 |
| --- | --- |
| Space / R | 시간 정지·재개 / 처음으로 되돌리기 |
| Tab / O | 느린 카메라 이동 / 전체 구도 |
| WASD·방향키 / 스크롤 | 카메라 이동 / 확대·축소 |
| F1 / H | 조작 안내 / 장면 이름·시간 표시 |
| 4 / 5 / 6 | frustum culling / CPU instancing / 병렬 명령 기록 |
| U / L / V | GPU culling / LOD / VSync |
| F5 / F6 / F7 | 같은 상태의 instancing A / B / 비교 지우기 |
| F11 / F12 | Profiler / RenderDoc 캡처 요청(빌드에서 활성화한 경우) |
| Escape | 창 닫기 |

데스크톱에서는 다른 창에 포커스를 옮겨도 장면은 계속 재생한다. 최소화하거나
그릴 크기가 없으면 렌더링을 중단한다. 모바일에서는 앱이 비활성 상태일 때도
새 GPU 작업을 멈춘다. Android 네이티브 창이 소실·교체되면 GPU 세션을 다시 만들고
CPU 장면·시각·사용자 설정은 유지한다. F11은 펼친 Profiler를 켜고 끄며, Profiler가
표시되는 동안 장면 이름 패널은 겹치지 않게 숨긴다.

터치 버튼은 Pause/Play·Restart·Tour·Overview·Profiler·Menu다. Menu에서
컬링·인스턴싱·기록 worker·GPU 컬링·LOD·VSync와 A/B 비교를 조작한다.
버튼 입력은 창 좌표를 framebuffer 좌표로 변환하며 Android 화면 밀도와 iOS의 Retina 배율을 반영한다.
모바일에서도 같은 렌더 기능과 기본 72개 캐릭터를 사용하므로 장치별 성능은 별도 확인이 필요하다.

창 크기가 바뀌면 화면을 먼저 확보한 뒤 모델을 준비한다. 모델 준비의 GPU
스키닝 제출이 스왑체인 재생성에 필요한 기존 제출 완료를 계속 밀어내지 않게 한다.
안내 글자는 창 너비에 따라 줄바꿈하며 가로로 압축하지 않는다.

F5는 시각·카메라·캐릭터 포즈를 고정하고 GPU culling·VSync·Profiler를 끈 뒤
CPU instancing OFF를 수집한다. 이후 장면 상태를 바꾸지 않고 F6을 누르면 ON을
수집한다. F7은 비교 표시를 지운다. 실제 효과와 프레임 시간은 실행 결과로 판단한다.
공통 비교 패널은 기존 `../PerformanceComparison.h`를 읽어 사용하며 수정하지 않는다.

## 파일과 확인할 내용

`main.cpp`는 장면·카메라·렌더링 연결, `DungeonScene.h`는 환경,
`ArenaActors.h`는 시간 기반 배치·연출, `ArenaModels.h`는 외부 모델·애니메이션,
`SceneHud.h`는 장면 표시를 담당한다. 에셋은 저장소 밖 `../assets/KayKit_Arena/`,
출처 설명은 [ASSET_CREDITS.md](ASSET_CREDITS.md)에 보관한다.

| 플랫폼 | 구현과 확인 범위 |
| --- | --- |
| Linux · Vulkan · Wayland | RTX 3060에서 빌드·실행. Profiler 180프레임, 검증 켠 크기 변경 240프레임 완료. 터치 UI 표시 300프레임·Profiler와 함께 표시 600프레임 완료. |
| Windows · Vulkan | 같은 GLSL 스키닝 경로. Windows 빌드·실행은 미확인. |
| Windows · D3D12 | HLSL Compute 스키닝 추가. Linux DXC의 `cs_6_0` 컴파일과 버퍼 배치 확인. Windows 실행은 미확인. |
| Android · Vulkan | 기존 네이티브 앱의 선택·에셋·터치·창 교체 경로 연결. Android 빌드·실기는 미확인. |
| iOS · Metal | Metal Compute 스키닝과 앱 선택·터치·Retina 경로 연결. Apple 도구 체인 컴파일·실기는 미확인. |

Linux 실행에서 사용하지 않는 vertex attribute 경고는 있었으며 validation error는 없었다.
가상 화면에는 포인터 장치가 없어 터치 버튼의 실제 누름은 확인하지 못했다.
검증용 가상 화면은 종료하며 실제 데스크톱 설정을 바꾸지 않는다.

## 다른 브랜치로 옮길 변경

엔진의 Compute 스키닝 지원 변경은 다음 네 파일로 독립되어 있다.
기존 공통 스키닝 수학·RHI 바인딩을 사용하며 `src/Public/` 변경은 없다.

- `src/Extends/Model/ModelShaderAssets.cpp`
- `src/Extends/Model/Shaders/ShaderAssets.cmake`
- `src/Extends/Model/Shaders/mesh_skinning_cs.hlsl`
- `src/Extends/Model/Shaders/mesh_skinning_cs.metal`

관련 API 안내는 `docs/component/guides.json`의 Compute 지원 범위를 함께 옮긴다.
Android 창 전환을 한 번에 합치지 않고 화면 밀도를 제공하는 수정은
`src/Platform/Windows/AndroidWindow.cpp`에 있다. 이 파일도 Public API 변경 없이
별도로 옮길 수 있다.
장면·빌드 연결을 옮길 때는 다음 파일들도 함께 옮긴다.

- `showcase/arena/`의 변경·추가 파일 전체.
- `CMakeLists.txt`, `cmake/Engine_Options.cmake`, `examples/CMakeLists.txt`의 Showcase 선택 연결.
- `showcase/CMakeLists.txt`, 새 `showcase/AssetDownload.cmake`, `showcase/ShowcaseAssets.cmake`, `showcase/README.md`.
- 모바일 전달 설정 `android/app/build.gradle`, `ios/build-libraries.sh`.

모바일은 기존 예제 registry·에셋 복사와 조합한다. 다른 작업의 Vulkan·렌더러
최적화나 Public 변경을 이 지원 변경의 필수 파일로 포함하지 않는다.

사용자가 확인할 항목은 모델과 텍스처 표시, 바닥·발 위치, 이동·공격 애니메이션,
조명과 그림자, 정지·재시작·카메라 전환, 동일 상태의 A/B 동작이다.
장치별 화질·성능과 60 FPS 달성 여부는 측정하지 않았다.
