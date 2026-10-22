# Android Examples 앱

Linux, macOS, Windows에서 같은 Gradle 프로젝트로 APK를 만든다. JDK 17과
Android SDK Command-line Tools를 준비하고 `ANDROID_HOME` 또는 Android Studio가
생성하는 `local.properties`의 `sdk.dir`로 SDK 위치를 지정한다.

프로젝트는 Gradle 9.6.0, Android Gradle Plugin 9.4.1, SDK 34,
Build Tools 36.0.0, NDK 28.2.13676358, CMake 3.22.1을 사용한다.
셰이더 컴파일러 `glslc`는 NDK의 호스트 도구에서 찾는다.

```sh
sdkmanager "platforms;android-34" "build-tools;36.0.0" \
    "ndk;28.2.13676358" "cmake;3.22.1" "platform-tools"
sdkmanager --licenses
cd android
./gradlew :app:assembleDebug
```

Windows에서는 `./gradlew` 대신 `gradlew.bat`을 사용한다. 결과는
`app/build/outputs/apk/debug/app-debug.apk`다. 기본 APK에는 `arm64-v8a`와
`x86_64` 네이티브 라이브러리가 들어가며 Android 14 이상에서 실행한다.
에뮬레이터용으로 한 ABI만 빌드하려면 `-PDY_ANDROID_ABIS=x86_64`를 추가한다.

Showcase Arena와 일반 예제를 함께 포함하려면 다음과 같이 빌드한다.
Arena에 필요한 에셋은 자동 준비되며 Cinematic의 큰 에셋은 받지 않는다.

```sh
./gradlew :app:assembleDebug \
    -PDY_BUILD_SHOWCASE=ON -PDY_SHOWCASE_ARENA_ONLY=ON
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

앱 아이콘으로 열면 예제 선택 화면이 나온다. 특정 예제로 바로 들어가려면
`dy_example` 문자열 intent extra에 CMake 타깃 이름을 전달한다.

```sh
adb shell am start -W -S \
    -n com.dyengine.app/android.app.NativeActivity \
    --es dy_example DyfPrimitives
adb shell am start -W -S \
    -n com.dyengine.app/android.app.NativeActivity \
    --es dy_example ShowcaseArena
```

`-S`는 기존 앱을 종료한 뒤 새 intent로 시작한다. 예제 진입 함수가 호출되면
logcat의 `dy_engine` 태그에 `dy_example=TARGET state=started`가 기록된다.
선택한 예제가 반환하면 `state=stopped`와 반환 코드를 기록한다. 실행 증명에는
이 로그와 실제 렌더링 화면을 함께 사용한다. 직접 실행한 예제가 끝나면
Activity도 종료하며, 선택 화면에서 연 예제는 Android Back으로 목록에 돌아온다.
