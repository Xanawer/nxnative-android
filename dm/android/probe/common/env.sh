# Shared settings for the Phase 0 probes (docs/android/PLAN.md). Sourced by the scripts beside it.
# SDK / NDK locations can be overridden from the environment.
ANDROID_HOME=${ANDROID_HOME:-$HOME/Library/Android/sdk}
NDK=${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.1.12297006}
API=${PROBE_API:-35}   # minSdk and the clang target; the manifest's targetSdk is 36 (exec-memory and tagging rules follow targetSdk)
TC=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
CC=$TC/aarch64-linux-android$API-clang
BT=$ANDROID_HOME/build-tools/36.1.0
ANDROID_JAR=$ANDROID_HOME/platforms/android-36/android.jar
ADB=$ANDROID_HOME/platform-tools/adb
KEYSTORE=${PROBE_KEYSTORE:-$HOME/.android/debug.keystore}   # never in the repository
CFLAGS_PROBE="-O1 -g -Wall -Wno-unused-function -fno-omit-frame-pointer"
die() { echo "error: $*" >&2; exit 1; }
