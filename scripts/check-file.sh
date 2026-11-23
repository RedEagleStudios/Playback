#!/usr/bin/env bash
# Syntax-check source files with the exact flags xmake compiles them with, without building or linking the DLL.
#
#   scripts/check-file.sh src/playback/replay/ReplaySession.cpp [more files...]
#
# A header is checked through the .cpp next to it (Foo.h -> Foo.cpp). The flags come from compile_commands.json, which
# is generated (xmake project -k compile_commands) when missing, when xmake.lua or the xmake configuration is newer, or
# when it has no entry for a requested source. The precompiled header from the last build is used when it is up to
# date; otherwise src/pch.h is included as text, which is slower.
# On Linux the Windows SDK comes from the xmake config's --sdk (xwin), unless INCLUDE is already set.
set -euo pipefail

caller=$PWD
root=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
cd "$root"

if [[ $# -eq 0 ]]; then
    echo "usage: scripts/check-file.sh <file.cpp|file.h>..." >&2
    exit 2
fi

sources=()
for file in "$@"; do
    [[ $file == /* ]] || file="$caller/$file"
    source=$(realpath -m --relative-to="$root" "$file")
    case $source in *.h | *.hpp) source="${source%.*}.cpp" ;; esac
    sources+=("$source")
done

conf=$(ls .xmake/*/*/xmake.conf 2>/dev/null | head -n 1 || true)
generate() {
    echo "check-file: generating compile_commands.json" >&2
    xmake project -k compile_commands -y >/dev/null
}
if [[ ! -f compile_commands.json || xmake.lua -nt compile_commands.json ]]; then
    generate
elif [[ -n "$conf" && "$conf" -nt compile_commands.json ]]; then
    generate
fi

winsysroot=""
if [[ "$(uname -s)" != MINGW* && "$(uname -s)" != MSYS* && -z "${INCLUDE:-}" ]]; then
    if [[ -n "$conf" ]]; then
        winsysroot=$(sed -n 's/^ *sdk = "\(.*\)",\{0,1\}$/\1/p' "$conf" | head -n 1)
    fi
    if [[ -z "$winsysroot" ]]; then
        echo "check-file: no Windows SDK; configure with xmake f --sdk=<xwin dir> or set INCLUDE" >&2
        exit 2
    fi
fi

check() {
python3 - "$winsysroot" "$@" <<'PY'
import json, os, subprocess, sys, time

root = os.getcwd()
winsysroot, files = sys.argv[1], sys.argv[2:]
entries = {}
for entry in json.load(open("compile_commands.json")):
    path = os.path.relpath(os.path.join(entry["directory"], entry["file"]))
    entries[path] = entry

# A source the database does not list yet (added since it was generated): exit 75 so the caller regenerates it once.
missing = [source for source in files if os.path.normpath(source) not in entries]
if missing and os.environ.get("CHECK_FILE_REGENERATED") != "1":
    sys.exit(75)


def outside_repo(directory, include):
    included = os.path.normcase(os.path.abspath(os.path.join(directory, include)))
    return not included.startswith(os.path.normcase(root) + os.sep)


status = 0
for source in files:
    entry = entries.get(os.path.normpath(source))
    if entry is None:
        print(f"check-file: {source}: no compile command (is it a source of the playback target?)", file=sys.stderr)
        status = 1
        continue
    directory, arguments = entry["directory"], entry["arguments"]

    # compile_commands.json leaves out the build's -Fp: the precompiled header sits next to the objects, in
    # <objectdir>/gens/src/cxx/pch.h.pch. Use it only while it matches this command: newer than src/pch.h, and built
    # with every flag and include directory listed here (its xmake depfile records them). A header built for another
    # mode or package version would fail with redefinitions, so src/pch.h is included as text instead.
    objectfile = next((arg[3:] for arg in arguments if arg.startswith(("-Fo", "/Fo"))), "").replace("\\", "/")
    objectdir = objectfile.split("/src/", 1)[0] if "/src/" in objectfile else ""
    pch = os.path.join(directory, objectdir, "gens", "src", "cxx", "pch.h.pch")
    pch_depfile = os.path.join(directory, objectdir.replace("/.objs/", "/.deps/", 1), "gens", "src", "cxx", "pch.h.pch.d")

    args = []
    for arg in arguments[1:]:
        if arg in ("-c", "/c") or arg.startswith(("-Fo", "/Fo", "-Fd", "/Fd", "-Fp", "/Fp")):
            continue
        if os.path.relpath(os.path.join(directory, arg)) == os.path.normpath(source):
            continue
        args.append(arg)

    use_pch = (
        bool(objectdir)
        and os.path.isfile(pch)
        and os.path.isfile(pch_depfile)
        and os.path.getmtime(pch) >= os.path.getmtime("src/pch.h")
    )
    if use_pch:
        recorded = open(pch_depfile, encoding="utf-8", errors="replace").read().replace("\\\\", "\\")
        use_pch = all(
            (arg[2:] if arg.startswith("-I") else arg) in recorded
            for arg in args
            if not arg.startswith(("-Yu", "/Yu", "-FI", "/FI"))
        )
    if not use_pch:
        args = [arg for arg in args if not arg.startswith(("-Yu", "/Yu"))]
    # The build passes package headers as external (/external:I, /external:W0); keep their warnings out.
    args = ["/external:I" + arg[2:] if arg.startswith("-I") and outside_repo(directory, arg[2:]) else arg for arg in args]
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
    note = "" if use_pch else " (without a matching precompiled header: build once to make checks faster)"
    print(f"check-file: {source}: {verdict} in {elapsed:.1f}s{note}", file=sys.stderr)
    status = status or result.returncode
sys.exit(status)
PY
}

status=0
check "${sources[@]}" || status=$?
if [[ $status -eq 75 ]]; then
    generate
    status=0
    CHECK_FILE_REGENERATED=1 check "${sources[@]}" || status=$?
fi
exit $status
