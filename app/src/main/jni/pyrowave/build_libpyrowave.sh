#!/bin/sh
# Rebuilds the prebuilt lib/arm64-v8a/libpyrowave-shared.so, which app/build.gradle packages,
# and pyrowave.h in this folder, from a PyroWave checkout (https://github.com/Themaister/pyrowave). Run after
# updating PyroWave. The Vulkan renderer loads the library at runtime, so the codec just isn't
# offered where it's missing.
#
#   PYROWAVE_DIR  PyroWave checkout (default: upstream/ in this folder). Granite is checked out
#                 at the revision pinned by PyroWave's checkout_granite.sh.
#   ANDROID_NDK   NDK to build with (default: the version app/build.gradle uses, in ANDROID_HOME
#                 or the default SDK location)
#   NINJA         ninja executable, if it isn't on the PATH
#
# Only arm64-v8a is built: PyroWave needs Vulkan 1.1 with extensions that no 32-bit or x86
# Android device we'd stream to has.
set -e

cd "$(dirname "$0")"
HERE=$(pwd)
REPO=$(cd ../../../../.. && pwd)

USE_PINNED_SOURCE=0
if [ -z "${PYROWAVE_DIR:-}" ]; then
    PYROWAVE_DIR=$HERE/upstream
    USE_PINNED_SOURCE=1
fi
if [ ! -e "$PYROWAVE_DIR/.git" ]; then
    echo "PyroWave source missing at $PYROWAVE_DIR; run git submodule update --init app/src/main/jni/pyrowave/upstream" >&2
    exit 1
fi
PYROWAVE_DIR=$(cd "$PYROWAVE_DIR" && pwd)

if [ "$USE_PINNED_SOURCE" = 1 ]; then
    (cd "$PYROWAVE_DIR" && ./checkout_granite.sh)
fi

NDK_VERSION=$(sed -n 's/.*ndkVersion "\(.*\)".*/\1/p' "$REPO/app/build.gradle")
if [ -z "$ANDROID_NDK" ]; then
    SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$LOCALAPPDATA/Android/Sdk}}
    ANDROID_NDK=$SDK/ndk/$NDK_VERSION
fi
if [ ! -d "$ANDROID_NDK" ]; then
    echo "NDK not found at $ANDROID_NDK; set ANDROID_NDK" >&2
    exit 1
fi

NINJA=${NINJA:-ninja}
STRIP=$(ls "$ANDROID_NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip* | head -n 1)

BUILD=$PYROWAVE_DIR/build-android-arm64
cmake -S "$PYROWAVE_DIR" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=Release
"$NINJA" -C "$BUILD" pyrowave-shared

LIBS=$HERE/lib/arm64-v8a
mkdir -p "$LIBS"
"$STRIP" --strip-unneeded -o "$LIBS/libpyrowave-shared.so" "$BUILD/libpyrowave-shared.so"
cp "$PYROWAVE_DIR/pyrowave.h" pyrowave.h

# Which PyroWave (and Granite) these came from
{
    (cd "$PYROWAVE_DIR" && git log -1 --format='%H %s')
    (cd "$PYROWAVE_DIR/Granite" && git log -1 --format='Granite %H %s')
} > SOURCE

echo "Updated $LIBS and $HERE from $PYROWAVE_DIR ($(cat SOURCE))"
