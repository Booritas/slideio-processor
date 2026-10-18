#!/usr/bin/env bash
# Builds slideio-processor from a fresh or existing clone:
#   1. initialises the git submodules (extern/slideio),
#   2. builds and installs SlideIO from the submodule into build/slideio-install,
#   3. installs this project's Conan dependencies,
#   4. configures and builds this project.
# Run the tests afterwards with ./run-tests.sh.
#
# Usage: ./build.sh [options]
#   -c, --config <release|debug>  Configuration of this project (default: release).
#                                 SlideIO is always installed in release; the
#                                 project uses only its headers.
#       --rebuild-slideio         Reinstall SlideIO even if the installed copy
#                                 matches the submodule commit.
#       --skip-slideio            Do not touch the SlideIO install.
#   -h, --help                    Show this help.
#
# Environment:
#   CONAN   Path to the Conan 2 executable, when `conan` is not on PATH.
#   PYTHON  Python 3 interpreter for SlideIO's install.py, when auto-detection fails.
#
# Works in Git Bash on Windows and in bash on Linux and macOS.

set -euo pipefail

usage() { sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "build.sh: $*" >&2; exit 1; }
step() { echo; echo "==> $*"; }

config=release
rebuild_slideio=0
skip_slideio=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) [[ $# -ge 2 ]] || die "$1 needs a value"; config="$2"; shift 2 ;;
        --rebuild-slideio) rebuild_slideio=1; shift ;;
        --skip-slideio) skip_slideio=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option '$1' (see --help)" ;;
    esac
done
case "$config" in
    release) build_type=Release; build_dir=build ;;
    debug) build_type=Debug; build_dir=build-debug ;;
    *) die "--config must be release or debug, not '$config'" ;;
esac

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

step "Initialising submodules"
git submodule update --init --recursive
git submodule status --recursive

# --- 2. SlideIO ----------------------------------------------------------------

slideio_src="$root/extern/slideio"
slideio_prefix="$root/build/slideio-install"
slideio_stamp="$slideio_prefix/.slideio-commit"

if [[ $skip_slideio -eq 1 ]]; then
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
"$conan" install . -of "$build_dir/conan" -s build_type="$build_type" -s compiler.cppstd=17 --build=missing

step "Configuring ($build_type)"
cmake -S . -B "$build_dir" \
      -DCMAKE_TOOLCHAIN_FILE="$(native "$root/$build_dir/conan/conan_toolchain.cmake")" \
      -DCMAKE_PREFIX_PATH="$(native "$slideio_prefix/release")" \
      -DCMAKE_BUILD_TYPE="$build_type"

step "Building ($build_type)"
cmake --build "$build_dir" --config "$build_type" --parallel

step "Done: $build_dir ($build_type). Run the tests with ./run-tests.sh -c $config"
