#!/bin/sh
# Xcode builds the common examples registry for its selected SDK/architecture.
set -eu
PATH="/opt/homebrew/bin:/usr/local/bin:/opt/local/bin:$PATH"
export PATH
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
command -v cmake >/dev/null || { echo "error: CMake is required."; exit 1; }
command -v ninja >/dev/null || { echo "error: Ninja is required."; exit 1; }
arch=${ARCHS%% *}
build_dir="$repo_dir/build/ios-$PLATFORM_NAME-$arch-$CONFIGURATION"
cmake -S "$script_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SDKROOT" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IPHONEOS_DEPLOYMENT_TARGET" \
    -DCMAKE_BUILD_TYPE="$CONFIGURATION"
cmake --build "$build_dir" --target dyf_examples --parallel 4

set --
while IFS= read -r archive; do
    if [ -n "$archive" ]; then
        set -- "$@" "$archive"
    fi
done < "$build_dir/ios-libraries-$CONFIGURATION.txt"
if [ "$#" -eq 0 ]; then
    echo "error: The examples static-library manifest is empty."
    exit 1
fi
xcrun libtool -static -o "$build_dir/lib/libdyf_ios_examples.a" "$@"

asset_dir="$build_dir/example-assets"
bundle_assets="$TARGET_BUILD_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/examples"
mkdir -p "$bundle_assets"
# Only the application's generated examples resource directory is synchronized.
rsync -a --delete "$asset_dir/" "$bundle_assets/"
