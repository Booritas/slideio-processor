#!/usr/bin/env bash
# Runs the slideio-processor test suite of a build made by ./build.sh.
#
# Usage: ./run-tests.sh [options] [-- <extra ctest arguments>]
#   -c, --config <release|debug>  Configuration to test (default: release).
#   -R, --regex <regex>           Run only the tests whose names match <regex>.
#   -h, --help                    Show this help.
#
# Arguments after `--` go to ctest unchanged, for example
#   ./run-tests.sh -- --repeat until-fail:3
#
# Works in Git Bash on Windows and in bash on Linux and macOS.

set -euo pipefail

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; }
die() { echo "run-tests.sh: $*" >&2; exit 1; }

config=release
regex=
extra=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) [[ $# -ge 2 ]] || die "$1 needs a value"; config="$2"; shift 2 ;;
        -R|--regex) [[ $# -ge 2 ]] || die "$1 needs a value"; regex="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; extra=("$@"); break ;;
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

command -v ctest >/dev/null 2>&1 || die "ctest not found"
[[ -f "$build_dir/CTestTestfile.cmake" ]] || die "no $build_type build in $build_dir; run ./build.sh -c $config first"

args=(--test-dir "$build_dir" -C "$build_type" --output-on-failure)
if [[ -n "$regex" ]]; then
    args+=(-R "$regex")
fi
ctest "${args[@]}" ${extra[@]+"${extra[@]}"}
