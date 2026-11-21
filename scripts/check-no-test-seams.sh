#!/usr/bin/env bash
# Requirements: UR-BLD-4
# Checks that a build made with -DSLIDEIO_PROCESSOR_TEST_SEAMS=OFF exports no test seam:
# no exported symbol of either shared library may contain "Testing" (the *ForTesting
# seams and trackForCloseTesting).
#
# Usage: scripts/check-no-test-seams.sh <build dir>
# Exit:  0 no test seams exported; 1 a seam is exported; 2 a library or a symbol tool
#        is missing.
#
# Lists exports with nm -D (Linux), nm -gU (macOS), dumpbin -exports or llvm-nm / nm
# (Windows).

set -euo pipefail

die() { echo "check-no-test-seams.sh: $*" >&2; exit 2; }

[[ $# -eq 1 ]] || { echo "usage: $0 <build dir>" >&2; exit 2; }
bin="$1/bin"
[[ -d "$bin" ]] || die "no directory $bin"

# A Windows host: the libraries are DLLs.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=1 ;; *) windows=0 ;; esac
kernel="$(uname -s)"

find_dumpbin() {
    if command -v dumpbin >/dev/null 2>&1; then command -v dumpbin; return; fi
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    [[ -x "$vswhere" ]] || return 1
    local install tool
    install="$("$vswhere" -latest -products '*' -property installationPath 2>/dev/null | tr -d '\r')" || return 1
    [[ -n "$install" ]] || return 1
    install="$(cygpath -u "$install")"
    tool="$(ls "$install"/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe 2>/dev/null | tail -n 1)" || true
    [[ -n "$tool" && -x "$tool" ]] || return 1
    printf '%s\n' "$tool"
}

# exports <library>: prints the exported symbol names, one per line.
if [[ $windows -eq 1 ]]; then
    if dumpbin="$(find_dumpbin)"; then
        exports() { "$dumpbin" -nologo -exports "$(cygpath -m "$1")" | tr -d '\r'; }
    elif command -v llvm-nm >/dev/null 2>&1; then
        exports() { llvm-nm -g --defined-only "$1"; }
    elif command -v nm >/dev/null 2>&1; then
        exports() { nm -g --defined-only "$1"; }
    else
        die "no tool to list exports: need dumpbin, llvm-nm or nm on PATH"
    fi
elif [[ "$kernel" == Darwin ]]; then
    command -v nm >/dev/null 2>&1 || die "nm not found"
    exports() { nm -gU "$1"; }
else
    command -v nm >/dev/null 2>&1 || die "nm not found"
    exports() { nm -D --defined-only "$1"; }
fi

failed=0
for name in slideio-processor-data slideio-processor-engine; do
    lib="$(find "$bin" \( -name "$name.dll" -o -name "lib$name.so*" -o -name "lib$name*.dylib" \) \
           -type f 2>/dev/null | head -n 1)"
    [[ -n "$lib" ]] || die "library $name not found under $bin"
    # Keep a failing listing from passing silently: capture it, then search it.
    listing="$(exports "$lib")" || die "could not list the exports of $lib"
    [[ -n "$listing" ]] || die "empty export listing for $lib"
    while IFS= read -r symbol; do
        echo "$(basename "$lib"): $symbol"
        failed=1
    done < <(grep 'Testing' <<<"$listing" || true)
done

if [[ $failed -ne 0 ]]; then
    echo "check-no-test-seams.sh: test seams are exported" >&2
    exit 1
fi
echo "no test seams exported"
