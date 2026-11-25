#!/usr/bin/env bash
# Configures, builds and runs tests/data_only_consumer against a data-only build's
# Conan output: the project uses slideio-processor-data through add_subdirectory and
# finds neither SlideIO nor OpenCV (no prefix path to either).
#
# Usage: scripts/check-data-only-consumer.sh <conan output folder>   (build-data/conan)
# Exit:  0 the consumer built and ran; non-zero otherwise.

set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: $0 <conan output folder of a data-only build>" >&2; exit 2; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conan_dir="$(cd "$1" && pwd)"
toolchain="$conan_dir/conan_toolchain.cmake"
[[ -f "$toolchain" ]] || { echo "no $toolchain" >&2; exit 2; }
build="$root/build-data-consumer"

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; *) windows=0 ;; esac
# CMake wants Windows-style paths on Windows.
native() { if [[ $windows == 1 ]]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

# A fresh tree: a stale cache could keep an old toolchain or a found SlideIO.
rm -rf "$build"
cmake -S "$(native "$root/tests/data_only_consumer")" -B "$(native "$build")" \
      -DCMAKE_TOOLCHAIN_FILE="$(native "$toolchain")" -DCMAKE_BUILD_TYPE=Release \
      | tee "$build.configure.log"
grep -q "data-only build (no SlideIO, no OpenCV)" "$build.configure.log" \
    || { echo "configure did not report a data-only build" >&2; exit 1; }
rm -f "$build.configure.log"

cmake --build "$(native "$build")" --config Release --parallel

# The executable lands in the build root (Release/ under Visual Studio); the library in
# bin/ (bin/Release/). Look in each, and put each existing one on the library path.
exe=""
libdirs=""
for d in "$build" "$build/Release" "$build/bin" "$build/bin/Release"; do
    [[ -d "$d" ]] && libdirs="$libdirs:$d"
    if [[ -z "$exe" ]]; then
        for n in consumer consumer.exe; do
            if [[ -f "$d/$n" ]]; then exe="$d/$n"; break; fi
        done
    fi
done
[[ -n "$exe" ]] || { echo "consumer executable not found under $build" >&2; exit 1; }
libdirs="${libdirs#:}"

if [[ $windows == 1 ]]; then
    PATH="$libdirs:$PATH" "$exe"
else
    LD_LIBRARY_PATH="$libdirs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    DYLD_LIBRARY_PATH="$libdirs${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" "$exe"
fi
