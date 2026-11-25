#!/usr/bin/env bash
# Builds slideio-processor from a fresh or existing clone:
#   1. initialises the git submodules (extern/slideio),
#   2. builds and installs SlideIO from the submodule into build/slideio-install,
#   3. installs this project's Conan dependencies,
#   4. configures and builds this project.
# Run the tests afterwards with ./run-tests.sh (not after --no-test-seams).
#
# Usage: ./build.sh [options]
#   -c, --config <release|debug>  Configuration of this project (default: release).
#                                 SlideIO is always installed in release. The
#                                 engine links it across an STL-bearing API, so
#                                 debug is refused on Windows, where MSVC's
#                                 Debug and Release STL layouts differ.
#       --rebuild-slideio         Reinstall SlideIO even if the installed copy
#                                 matches the submodule commit.
#       --skip-slideio            Do not touch the SlideIO install.
#       --no-test-seams           Build without the test-only seams, as for a release
#                                 package; tests are not built. Writes build-noseams
#                                 (build-noseams-debug for debug).
#       --data-only               Build only the data library (core, config, storage,
#                                 reporting) and its tests, without SlideIO or OpenCV
#                                 (SLIDEIO_PROCESSOR_DATA_ONLY). SlideIO is not built,
#                                 and debug is allowed on Windows. Writes build-data
#                                 (build-data-debug for debug; build-noseams-data with
#                                 --no-test-seams). Test it with ./run-tests.sh --data-only.
#       --warnings-as-errors      Fail the build on any compiler warning in this
#                                 project's sources.
#   -h, --help                    Show this help.
#
# Environment:
#   CONAN   Path to the Conan 2 executable, when `conan` is not on PATH.
#   PYTHON  Python 3 interpreter for SlideIO's install.py, when auto-detection fails.
#
# Works in Git Bash on Windows and in bash on Linux and macOS.

set -euo pipefail

usage() { sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "build.sh: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

config=release
rebuild_slideio=0
skip_slideio=0
test_seams=ON
warnings_as_errors=OFF
data_only=OFF
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) [[ $# -ge 2 ]] || die "$1 needs a value"; config="$2"; shift 2 ;;
        --rebuild-slideio) rebuild_slideio=1; shift ;;
        --skip-slideio) skip_slideio=1; shift ;;
        --no-test-seams) test_seams=OFF; shift ;;
        --data-only) data_only=ON; shift ;;
        --warnings-as-errors) warnings_as_errors=ON; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1' (see --help)" ;;
    esac
done
case "$config" in
    release) build_type=Release; build_dir=build ;;
    debug) build_type=Debug; build_dir=build-debug ;;
    *) die "--config must be release or debug, not '$config'" ;;
esac
# A data-only build has its own tree (build-data, build-data-debug), as has a build without
# the seams, so neither replaces the build the tests use.
if [[ "$data_only" == ON ]]; then build_dir="build-data${build_dir#build}"; fi
if [[ "$test_seams" == OFF ]]; then build_dir="build-noseams${build_dir#build}"; fi
# SlideIO's API passes std::string, std::vector and std::shared_ptr across its DLL
# boundary. MSVC lays those out differently in Debug and Release and nothing detects
# the mismatch, so a Debug build against the Release SlideIO installed below would
# be undefined behaviour. SlideIO's install.py can build Debug, but that is a second
# full SlideIO build plus every Conan dependency rebuilt in Debug, so it is not
# offered here; CMake refuses the same mismatch (cmake/check_slideio_configuration.cmake).
# A data-only build links no SlideIO, so it has no such boundary.
if [[ "$config" == debug && "$data_only" == OFF ]]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            die "--config debug is not supported on Windows: the Debug engine would link the Release"                 "SlideIO across an STL-bearing API, whose layout differs between MSVC Debug and Release."                 "Build release (the default), or install SlideIO in Debug yourself and configure against it." ;;
    esac
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$root"

# Native tools (cmake, conan, python) on Windows need Windows paths, not /d/... ones.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }

# --- Tools -------------------------------------------------------------------

find_conan() {
    if [[ -n "${CONAN:-}" ]]; then
        [[ -x "$CONAN" ]] || die "CONAN=$CONAN is not executable"
        printf '%s\n' "$CONAN"
        return
    fi
    if command -v conan >/dev/null 2>&1; then
        command -v conan
        return
    fi
    # A conda environment holding Conan, as on the reference Windows setup.
    local candidate
    for candidate in "$HOME"/anaconda3/envs/*/Scripts/conan.exe "$HOME"/miniconda3/envs/*/Scripts/conan.exe \
                     "$HOME"/anaconda3/envs/*/bin/conan "$HOME"/miniconda3/envs/*/bin/conan; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    die "Conan 2 not found; put it on PATH or set CONAN=/path/to/conan"
}

find_python() {
    if [[ -n "${PYTHON:-}" ]]; then
        printf '%s\n' "$PYTHON"
        return
    fi
    # The Windows Store alias for python exists but does not run, so try each one.
    local candidate
    for candidate in python3 python "py -3"; do
        if $candidate -c 'import sys; sys.exit(sys.version_info[0] != 3)' >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return
        fi
    done
    die "Python 3 not found; put it on PATH or set PYTHON=/path/to/python"
}

command -v git >/dev/null 2>&1 || die "git not found"
command -v cmake >/dev/null 2>&1 || die "cmake not found"
conan="$(find_conan)"
# SlideIO's install.py runs `conan` by name.
PATH="$(dirname "$conan"):$PATH"
export PATH
echo "conan:  $conan ($("$conan" --version))"
echo "cmake:  $(command -v cmake) ($(cmake --version | head -n 1))"

# --- 1. Submodules -------------------------------------------------------------

if [[ "$data_only" == ON ]]; then
    step "Skipping submodules (--data-only)"
else
    step "Initialising submodules"
    git submodule update --init --recursive
    git submodule status --recursive
fi

# --- 2. SlideIO ----------------------------------------------------------------

slideio_src="$root/extern/slideio"
slideio_prefix="$root/build/slideio-install"
slideio_stamp="$slideio_prefix/.slideio-commit"

if [[ "$data_only" == ON ]]; then
    step "Skipping SlideIO (--data-only)"
elif [[ $skip_slideio -eq 1 ]]; then
    step "Skipping SlideIO (--skip-slideio)"
else
    commit="$(git -C "$slideio_src" rev-parse HEAD)"
    if [[ -n "$(git -C "$slideio_src" status --porcelain)" ]]; then
        commit="$commit-dirty" # local edits: never trust a previous install
    fi
    installed="$(cat "$slideio_stamp" 2>/dev/null || true)"
    if [[ $rebuild_slideio -eq 0 && "$installed" == "$commit" && "$commit" != *-dirty \
          && -d "$slideio_prefix/release" ]]; then
        step "SlideIO ${commit:0:12} is already installed in build/slideio-install"
    else
        python="$(find_python)"
        step "Building and installing SlideIO ${commit:0:12} (this takes a while)"
        # install.py resolves a relative prefix against its own directory, so pass an absolute one.
        (cd "$slideio_src" && $python install.py -a install -c release --prefix "$(native "$slideio_prefix")")
        [[ -d "$slideio_prefix/release" ]] || die "SlideIO install did not produce $slideio_prefix/release"
        printf '%s\n' "$commit" > "$slideio_stamp"
    fi
fi

# --- 3-4. This project --------------------------------------------------------------

step "Installing Conan dependencies ($build_type)"
# A fresh machine (a CI runner, a new container) has no default profile yet.
if ! "$conan" profile path default >/dev/null 2>&1; then
    "$conan" profile detect
fi
# The default profile's arch is whatever the first `profile detect` saw, and a restored
# ~/.conan2 cache can carry the wrong one; SlideIO's install.py picks its profile from
# the host, so on macOS name the host arch here or the two will not link.
arch_setting=()
if [[ "$(uname -s)" == Darwin ]]; then
    case "$(uname -m)" in
        arm64) arch_setting=(-s arch=armv8) ;;
        x86_64) arch_setting=(-s arch=x86_64) ;;
    esac
fi
# A data-only build needs no OpenCV package ("&:" scopes the option to this conanfile).
data_only_option=()
if [[ "$data_only" == ON ]]; then data_only_option=(-o "&:data_only=True"); fi
# ${a[@]+...} because macOS's bash 3.2 treats an empty array as unbound under set -u.
"$conan" install . -of "$build_dir/conan" -s build_type="$build_type" -s compiler.cppstd=17 \
    ${arch_setting[@]+"${arch_setting[@]}"} ${data_only_option[@]+"${data_only_option[@]}"} --build=missing

step "Configuring ($build_type)"
# A data-only build must not find SlideIO, so it gets no prefix to the SlideIO install.
prefix_path=()
if [[ "$data_only" == OFF ]]; then prefix_path=(-DCMAKE_PREFIX_PATH="$(native "$slideio_prefix/release")"); fi
cmake -S . -B "$build_dir" \
      -DCMAKE_TOOLCHAIN_FILE="$(native "$root/$build_dir/conan/conan_toolchain.cmake")" \
      ${prefix_path[@]+"${prefix_path[@]}"} \
      -DCMAKE_BUILD_TYPE="$build_type" \
      -DSLIDEIO_PROCESSOR_DATA_ONLY="$data_only" \
      -DSLIDEIO_PROCESSOR_TEST_SEAMS="$test_seams" \
      -DSLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS="$warnings_as_errors"

step "Building ($build_type)"
cmake --build "$build_dir" --config "$build_type" --parallel

if [[ "$test_seams" == OFF ]]; then
    step "Done: $build_dir ($build_type), no test seams. Check it with scripts/check-no-test-seams.sh $build_dir"
elif [[ "$data_only" == ON ]]; then
    step "Done: $build_dir ($build_type), data only. Run the tests with ./run-tests.sh --data-only -c $config"
else
    step "Done: $build_dir ($build_type). Run the tests with ./run-tests.sh -c $config"
fi
