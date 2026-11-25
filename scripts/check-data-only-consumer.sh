#!/usr/bin/env bash
# Configures, builds and runs tests/data_only_consumer against a data-only build's
# Conan output: the project uses slideio-processor-data through add_subdirectory and
# finds neither SlideIO nor OpenCV (no prefix path to either).
#
# Usage: scripts/check-data-only-consumer.sh <conan output folder>   (build-data/conan)
#        Release builds only: the folder must be a Release build's Conan output.
# Exit:  0 the consumer built and ran; non-zero otherwise.

set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: $0 <conan output folder of a data-only build>" >&2; exit 2; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
conan_dir="$(cd "$1" && pwd)"
toolchain="$conan_dir/conan_toolchain.cmake"
[[ -f "$toolchain" ]] || { echo "no $toolchain" >&2; exit 2; }
build="$root/build-data-consumer"
case "$conan_dir" in
    *debug*) echo "$0: Release builds only; $conan_dir looks like a Debug build" >&2; exit 2 ;;
esac

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; *) windows=0 ;; esac
# CMake wants Windows-style paths on Windows.
native() { if [[ $windows == 1 ]]; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

# A fresh tree: a stale cache could keep an old toolchain or a found SlideIO.
rm -rf "$build"
mkdir -p "$build"
log="$build/configure.log"
cmake -S "$(native "$root/tests/data_only_consumer")" -B "$(native "$build")" \
      -DCMAKE_TOOLCHAIN_FILE="$(native "$toolchain")" -DCMAKE_BUILD_TYPE=Release \
      | tee "$log"
grep -q "data-only build (no SlideIO, no OpenCV)" "$log" \
    || { echo "configure did not report a data-only build" >&2; exit 1; }

cmake --build "$(native "$build")" --config Release --parallel

# The executable lands in the build root (Release/ under Visual Studio). On Windows the
# POST_BUILD step copies the DLLs beside it, and the run adds no library directory to
# PATH, which proves the copy works. Elsewhere the build-tree RPATH finds the library, so
# LD_LIBRARY_PATH/DYLD_LIBRARY_PATH are not set either.
exe=""
for d in "$build" "$build/Release"; do
    for n in consumer consumer.exe; do
        if [[ -z "$exe" && -f "$d/$n" ]]; then exe="$d/$n"; fi
    done
done
[[ -n "$exe" ]] || { echo "consumer executable not found under $build" >&2; exit 1; }

"$exe"
