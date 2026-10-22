# Showcase 구성과 에셋

Linux·Wayland에서 Bistro를 실행하는 예:

```sh
./build/showcase/showcase exterior
./build/showcase/showcase interior
./build/showcase/showcase night
./build/showcase/showcase emerald
```

발표의 장면별 역할·구도·조작과 탐색 기록은 [발표 초안](Presentation.md)에 정리한다.
현재 RTX 3060·Sway 환경에서는 Wayland 경로를 사용하며, 화면을 방해하지 않는 탐색에는
실제 데스크톱과 분리된 임시 headless Wayland 환경을 사용했다.
headless 완료 처리량은 실제 작업 공간의 화면 표시 FPS와 구분한다.

Linux 하이브리드 노트북에서 NVIDIA를 별도로 지정해야 할 때의 실행 예:

```sh
__NV_PRIME_RENDER_OFFLOAD=1 __VK_LAYER_NV_optimus=NVIDIA_only ./build/showcase/showcase night
```

`exterior`는 기존 낮 장면, `night` 또는 `exterior --night`는 별도 야간 프리셋이다.
야간은 어두운 하늘·달빛과 전등 위치의 점/스폿·LTC 면광원을 사용하며,
낮 장면과 FPS를 같은 조건으로 비교하지 않는다. 과거 RTX 4050·NVIDIA 550 환경에서는
Wayland swapchain 생성이 실패했고 `DY_LINUX_WINDOW_SYSTEM=X11` 빌드의
XWayland 출력은 동작했다. 이 기록은 현재 RTX 3060 환경과 다른 드라이버·compositor 조합의 결과다.

`DY_BUILD_SHOWCASE=ON`으로 Arena·Culling·Cinematic을 선택한다.
필요한 Model·AdvancedRendering도 함께 활성화한다.
구성 단계에서 누락된 기본 에셋을 자동으로 다운로드하며, 준비된 파일은 재사용한다.
일반 예제는 `DY_BUILD_EXAMPLES`로 따로 선택하며, 처음 구성할 때 Showcase 선택의
기본값은 `DY_BUILD_EXAMPLES`를 따른다. 이미 캐시에 지정된 선택은 유지한다.
`DY_SHOWCASE_ARENA_ONLY=ON`이면 Showcase에서 Arena만 포함하며 Culling·Cinematic의
구성과 큰 외부 에셋 준비를 생략한다. 기본값은 OFF다.

## 플랫폼 선택

Windows는 `USE_D3D12=ON` 또는 `USE_VULKAN=ON` 중 하나를 명시한다.
백엔드를 바꿀 때는 기존 `build/` 내용을 비우고 다시 구성한다. Linux·Android는 Vulkan,
iOS는 Metal 경로를 사용한다. Arena의 Model Compute 스키닝 셰이더는
GLSL·HLSL·Metal 모두 제공한다.

Android의 기존 앱에 Arena만 포함하려면 `android/`에서 다음과 같이 구성한다.

```sh
./gradlew :app:assembleDebug -PDY_BUILD_SHOWCASE=ON -PDY_SHOWCASE_ARENA_ONLY=ON
```

Gradle 속성 두 개는 CMake에 같은 이름으로 전달되며 모두 기본 OFF다.
Showcase를 선택하면 필요한 에셋도 자동으로 준비한다.
패키지에는 선택한 예제의 에셋과 라이선스를 기존 manifest 경로로 넣는다.

iOS는 기존 Xcode 프로젝트의 build setting에 `DY_BUILD_SHOWCASE=ON`,
`DY_SHOWCASE_ARENA_ONLY=ON`을 지정한다. `ios/build-libraries.sh`가
이 설정을 CMake로 전달하고 생성한 예제 에셋을 앱 bundle로 복사한다.
모바일은 독립 `showcase/arena` 실행 파일 대신 기존 네이티브 앱을 사용한다.

플랫폼별 구현·실행 확인 범위와 조작은 [Arena 안내](arena/README.md)를 따른다.

## 자동 다운로드

Showcase를 구성하면 선택한 장면에 필요한 기본 에셋을 자동으로 준비한다.
기존 로컬 파일과 ZIP은 재사용하고, 누락된 파일만 다운로드한다.
`DY_BUILD_SHOWCASE=OFF`이면 Showcase 에셋을 준비하지 않는다.
Arena 폴더를 독립 프로젝트로 구성할 때도 필요한 KayKit 에셋을 자동으로 준비한다.

저장소 루트에서 Linux·Wayland용 Showcase를 구성하는 예:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDY_LINUX_WINDOW_SYSTEM=WAYLAND -DDY_BUILD_SHOWCASE=ON
cmake --build build --target ShowcaseArena ShowcaseCulling ShowcaseCinematic -j
```

실행 파일은 `build/showcase/arena`, `build/showcase/culling`, `build/showcase/showcase`에 생성한다.

- 저장 위치는 `DY_ASSET_ROOT`이며 기본값은 저장소와 나란한 `../assets/`다.
- Arena는 `KayKit_Arena/`, Cinematic은 `Bistro_v5_2/`, `EmeraldSquare_v4_1/`,
  `Intel_Animated_Knight/`를 사용한다.
- Sponza는 자동 다운로드 대상에 포함하지 않는다. 비교용 장면이 필요하면
  `DY_SHOWCASE_COMPARISON_SCENE`에 준비된 로컬 파일을 지정한다.
- 기존에 준비된 파일이 있으면 네트워크에 접속하지 않는다. 원본 ZIP이 있으면
  다시 받지 않고 추출한다. 불완전한 폴더에는 없는 파일만 추가한다.
- KayKit은 공식 저장소의 고정 commit에서 모델·텍스처·라이선스를 받는다.
  대형 장면은 NVIDIA·Intel 공식 배포 ZIP을 사용하며 원문 라이선스도 함께 추출한다.
- 전체 Cinematic 원본은 수 GB다. Arena만 구성하면 KayKit 파일만 준비하고,
  Culling은 외부 에셋을 받지 않는다. 미연결 Jungle Ruins는 대상에 포함하지 않는다.
- 사용자가 별도로 지정한 Cinematic 모델 경로에는 기본 패키지를 내려받지 않는다.
  해당 파일은 직접 준비한다. Arena의 `DY_ARENA_GAME_ASSETS`는 KayKit 다운로드
  위치도 지정한다.
- 다운로드 중간 파일·추출 작업은 `${DY_ASSET_ROOT}/.download/`에 생성하고
  성공 후 정리한다. 동시 구성용 잠금 파일은 그 폴더에 유지한다.

GLFW·stb·meshoptimizer 등 라이브러리의 FetchContent 설정은 에셋 준비와 별개다.

장면 출처와 이용 조건은 [Cinematic](cinematic/ASSET_CREDITS.md)과
[Arena](arena/ASSET_CREDITS.md)에 기록한다.
