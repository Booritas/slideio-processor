#!/usr/bin/env bash
# Requirements: UR-BLD-2
# Checks that a build made with -DSLIDEIO_PROCESSOR_TEST_SEAMS=OFF exports no test seam.
# Two kinds of exported symbol of either shared library fail it: one containing "Testing"
# (the *ForTesting seams and trackForCloseTesting), and one containing the name of a
# test-only export (SLIDEIO_PROCESSOR_ENGINE_TEST_EXPORTS in the engine library,
# SLIDEIO_PROCESSOR_DATA_TEST_EXPORTS in the data library), listed in test_only below.
# The three SeamMergerAccess functions are matched by their owner, the others by name.
#
# Usage: scripts/check-no-test-seams.sh <build dir>
# Exit:  0 no test seams exported; 1 a seam is exported; 2 a library or a symbol tool
#        is missing.
#
# Lists exports with nm -D (Linux), nm -gU (macOS). On Windows: dumpbin -exports (Visual
# Studio; found through vswhere when not on PATH), else llvm-readobj --coff-exports (LLVM;
# reads the PE export table, so it suits MSVC DLLs), else nm as a last resort. nm reads
# the COFF symbol table, which MSVC DLLs lack, so it only suits MinGW-built DLLs and
# otherwise yields an empty listing, which exits 2.

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
        # Only the export-table rows ("ordinal hint RVA name"), not the "Dump of file
        # <path>" header, which a build path could make match.
        exports() {
            "$dumpbin" -nologo -exports "$(cygpath -m "$1")" | tr -d '\r' |
                awk '$1 ~ /^[0-9]+$/ && NF >= 4 { print $4 }'
        }
    elif command -v llvm-readobj >/dev/null 2>&1; then
        exports() {
            llvm-readobj --coff-exports "$(cygpath -m "$1")" | tr -d '\r' |
                awk '/^[[:space:]]*Name:/ { sub(/^[[:space:]]*Name:[[:space:]]*/, ""); print }'
        }
    elif command -v nm >/dev/null 2>&1; then
        exports() { nm -g --defined-only "$1"; }
    else
        die "no tool to list exports: need dumpbin, llvm-readobj or nm on PATH"
    fi
elif [[ "$kernel" == Darwin ]]; then
    command -v nm >/dev/null 2>&1 || die "nm not found"
    exports() { nm -gU "$1"; }
else
    command -v nm >/dev/null 2>&1 || die "nm not found"
    exports() { nm -D --defined-only "$1"; }
fi

# Detail functions that production calls only inside their own library; they are exported
# only when test seams are on, so tests can reach them. Each name is matched as a substring.
test_only=(
    SeamMergerAccess     # the owner of notifyTileSkipped, takeAwaitedDeferred and completeDeferred;
                         # matched by owner because SeamMerger::Impl has methods of the same
                         # names that GCC exports with the class
    stageRecordJson      # detail: serialises a stage definition for the stage registry
    stageFromRecord      # detail: parses a stored stage record back to a definition
    sameDeclaration      # detail: compares two stage declarations
    resolveColumns       # detail: resolves a reporting column schema against a store
    appendDouble         # detail: formats a CSV measurement value (reporting)
    requireDistinctNames # detail: refuses two reporting columns of the same name
)
pattern="Testing"
for name in "${test_only[@]}"; do pattern+="|$name"; done

failed=0
for name in slideio-processor-data slideio-processor-engine; do
    lib="$(find "$bin" \( -name "$name.dll" -o -name "lib$name.so*" -o -name "lib$name*.dylib" \) \
           -type f 2>/dev/null | head -n 1)" || true
    [[ -n "$lib" ]] || die "library $name not found under $bin"
    # Keep a failing listing from passing silently: capture it, then search it.
    listing="$(exports "$lib")" || die "could not list the exports of $lib"
    [[ -n "$listing" ]] || die "empty export listing for $lib"
    while IFS= read -r symbol; do
        echo "$(basename "$lib"): $symbol"
        failed=1
    done < <(grep -E "$pattern" <<<"$listing" || true)
done

if [[ $failed -ne 0 ]]; then
    echo "check-no-test-seams.sh: test seams are exported" >&2
    exit 1
fi
echo "no test seams exported"
