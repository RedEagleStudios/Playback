#!/usr/bin/env bash
# Syntax-check source files with the exact flags xmake compiles them with, without building or linking the DLL.
#
#   scripts/check-file.sh src/playback/replay/ReplaySession.cpp [more files...]
#
# A header is checked through the .cpp next to it (Foo.h -> Foo.cpp). The flags come from compile_commands.json, which
# is generated on first use and again whenever xmake.lua is newer (xmake project -k compile_commands). The precompiled
# header from the last build is used when it is up to date; otherwise src/pch.h is included as text, which is slower.
# On Linux the Windows SDK comes from the xmake config's --sdk (xwin), unless INCLUDE is already set.
set -euo pipefail

root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$root"

if [[ $# -eq 0 ]]; then
    echo "usage: scripts/check-file.sh <file.cpp|file.h>..." >&2
    exit 2
fi

if [[ ! -f compile_commands.json || xmake.lua -nt compile_commands.json ]]; then
    echo "check-file: generating compile_commands.json" >&2
    xmake project -k compile_commands -y >/dev/null
fi

winsysroot=""
if [[ "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* && -z "${INCLUDE:-}" ]]; then
    conf=$(ls .xmake/*/*/xmake.conf 2>/dev/null | head -n 1 || true)
    if [[ -n "$conf" ]]; then
        winsysroot=$(sed -n 's/^ *sdk = "\(.*\)",\{0,1\}$/\1/p' "$conf" | head -n 1)
    fi
    if [[ -z "$winsysroot" ]]; then
        echo "check-file: no Windows SDK; configure with xmake f --sdk=<xwin dir> or set INCLUDE" >&2
        exit 2
    fi
fi

python3 - "$winsysroot" "$@" <<'PY'
import json, os, subprocess, sys, time

root = os.getcwd()
winsysroot, files = sys.argv[1], sys.argv[2:]
entries = {}
for entry in json.load(open("compile_commands.json")):
    path = os.path.relpath(os.path.join(entry["directory"], entry["file"]))
    entries[path] = entry


def outside_repo(directory, include):
    included = os.path.normcase(os.path.abspath(os.path.join(directory, include)))
    return not included.startswith(os.path.normcase(root) + os.sep)


status = 0
for requested in files:
    source = os.path.relpath(requested)
    if source.endswith((".h", ".hpp")):
        source = os.path.splitext(source)[0] + ".cpp"
    entry = entries.get(source)
    if entry is None:
        print(f"check-file: {requested}: no compile command (is it a source of the playback target?)", file=sys.stderr)
        status = 1
        continue
    directory, arguments = entry["directory"], entry["arguments"]

    # compile_commands.json leaves out the build's -Fp: the precompiled header sits next to the objects, in
    # <objectdir>/gens/src/cxx/pch.h.pch. Use it while it is newer than src/pch.h, else include src/pch.h as text.
    objectfile = next((arg[3:] for arg in arguments if arg.startswith(("-Fo", "/Fo"))), "").replace("\\", "/")
    objectdir = objectfile.split("/src/", 1)[0] if "/src/" in objectfile else ""
    pch = os.path.join(directory, objectdir, "gens", "src", "cxx", "pch.h.pch")
    use_pch = bool(objectdir) and os.path.isfile(pch) and os.path.getmtime(pch) >= os.path.getmtime("src/pch.h")

    args = []
    for arg in arguments[1:]:
        if arg in ("-c", "/c") or arg.startswith(("-Fo", "/Fo", "-Fd", "/Fd", "-Fp", "/Fp")):
            continue
        if not use_pch and arg.startswith(("-Yu", "/Yu")):
            continue
        if os.path.relpath(os.path.join(directory, arg)) == source:
            continue
        # The build passes package headers as external (/external:I, /external:W0); keep their warnings out.
        if arg.startswith("-I") and outside_repo(directory, arg[2:]):
            arg = "/external:I" + arg[2:]
        args.append(arg)
    command = [arguments[0], *args, "/external:W0", "/Zs"]
    if use_pch:
        command.append("-Fp" + pch)
    if winsysroot:
        command += ["/winsysroot", winsysroot]
    command.append(source)

    started = time.monotonic()
    result = subprocess.run(command, cwd=directory)
    elapsed = time.monotonic() - started
    verdict = "ok" if result.returncode == 0 else "FAILED"
    note = "" if use_pch else " (no precompiled header yet: build once to make checks faster)"
    print(f"check-file: {source}: {verdict} in {elapsed:.1f}s{note}", file=sys.stderr)
    status = status or result.returncode
sys.exit(status)
PY
