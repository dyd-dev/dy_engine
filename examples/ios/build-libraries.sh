#!/bin/sh
# Xcode의 SDK·CPU·빌드 구성을 그대로 사용해 엔진과 advanced 07·08을 만든다.
# Xcode 빌드 단계에서 호출된다. 실기기·시뮬레이터 및 Debug·Release 산출물은 별도 폴더에 둔다.
set -eu
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:$PATH"
export PATH
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
command -v cmake >/dev/null || { echo "error: CMake가 필요합니다. CMake 설치 후 다시 빌드하세요."; exit 1; }
command -v ninja >/dev/null || { echo "error: Ninja가 필요합니다. Ninja 설치 후 다시 빌드하세요."; exit 1; }
arch=${ARCHS%% *}
build_dir="$repo_dir/build/ios-$PLATFORM_NAME-$arch-$CONFIGURATION"
cmake -S "$script_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION"
cmake --build "$build_dir" --target AdvancedPBRLighting AdvancedShadowMapping --parallel 4
