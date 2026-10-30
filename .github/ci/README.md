# 플랫폼별 빌드 CI

모든 브랜치의 push에서 데스크톱 단독 예제와 Android·iOS 앱을 Debug/Release로 빌드합니다. CI는 빌드 성공 여부만 판정합니다.

| 플랫폼 | 빌드 |
| --- | --- |
| Linux | Vulkan Debug/Release |
| Windows | Vulkan/D3D12 Debug/Release |
| macOS | Metal Debug/Release |
| Android | Gradle Debug/Release APK, arm64-v8a/x86_64 |
| iOS | Xcode Debug/Release, 실기기·시뮬레이터 arm64, 코드 서명 제외 |

데스크톱은 `DY_BUILD_EXAMPLES=ON`으로 구성한 전체 타깃을 빌드합니다. 예제는 `vertex.h`·`fragment.h`를 생성하지 않고 단독 실행 파일로 빌드합니다.

모바일은 네이티브 예제 앱을 빌드합니다. 선택적 런타임 GLSL 컴파일러는 기본값 OFF를 사용합니다. 앱 실행이나 예제의 런타임 셰이더 로딩은 검사하지 않습니다.

Android SDK·NDK·CMake 버전은 `android/app/build.gradle`에서 읽습니다. iOS는 기존 Xcode 프로젝트와 빌드 단계를 사용합니다. CI 전용 테스트 타깃이나 제품 소스 계측을 추가하지 않습니다.

빌드 실패나 필수 빌드 도구 부족은 해당 작업 실패로 표시합니다.

빌드 로그와 모바일 패키지(Android APK·iOS 시뮬레이터 앱)는 GitHub artifact로 7일 보관합니다.

수동 빌드는 기존 CI 파일을 사용합니다.

```sh
python -B .github/ci/ci.py --platform linux --phase build --build-dir build/ci
```

플랫폼 값은 `linux`, `windows`, `macos`, `android`, `ios`입니다. `--config Debug|Release`로 빌드 구성을 선택하며 기본값은 Debug입니다. Windows는 `--api vulkan|d3d12`를 지정합니다. 로컬 hook은 설치하지 않습니다.

문서 배포는 같은 workflow의 Pages 작업으로 유지합니다. main과 mobile-application push에서 `docs/`를 배포하며, 저장소 Pages Source는 GitHub Actions를 사용합니다.
