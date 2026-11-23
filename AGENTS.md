# Working on Playback

Playback is a LeviLamina client mod: one DLL (`playback`) built with xmake and clang-cl for Windows x64. On Linux it
cross-compiles with clang-cl, lld-link and an [xwin](https://github.com/Jake-Shadle/xwin) Windows SDK. Read
[CONTRIBUTING.md](CONTRIBUTING.md) and [docs/building.md](docs/building.md) for the release build.

## Build loop

Configure once for development, then build:

```sh
# Linux: add --toolchain=clang-cl --sdk=<xwin dir> (and --ffmpeg_exe=<ffmpeg.exe> to skip building FFmpeg).
xmake f -p windows -a x64 -m dev --target_type=client -y
xmake -y
```

- `-m dev` is the release build without optimisation: same defines and runtime, faster to compile. It also reruns
  LeviLamina's prelink (about 25 s under Wine) only when the set of game symbols the code uses changes. Use
  `-m release` for anything you ship or measure.
- After editing one `.cpp`, a dev build recompiles that file and relinks: seconds for a small file, about 30 s for
  `ReplaySession.cpp`. Editing a Playback header recompiles every source that includes it.
- `src/pch.h` is the precompiled header, force-included into every source. Keep it to stable third-party headers
  (LeviLamina, the standard library, imgui): changing it rebuilds every file.
- The build copies the mod to `bin/playback/` (well under a second). Copy that folder into the game's `mods/`.

## Check one file before building

`scripts/check-file.sh <file>...` runs clang-cl `/Zs` (syntax and semantic check, no code generation) on a source with
the flags xmake uses, read from `compile_commands.json` (generated on first use, and again when `xmake.lua` changes). A
header is checked through the `.cpp` next to it. With the precompiled header from a previous build it takes 0.1-5 s for
most files and about 12 s for `ReplaySession.cpp`, so run it on every file you touch before `xmake` and fix what it
reports first. It needs `python3`; on Windows run it from Git Bash. Prefer it to `xmake build --files=<file>`, which
does not build the precompiled header and fails with "PCH file not found" after a clean or a `src/pch.h` change.

## Minecraft and LeviLamina APIs: look, don't guess

Most failed builds come from guessing the shape of a Minecraft class. Before using a member, type or header, open it in
the LeviLamina headers of the version `xmake.lua` requires:

```sh
ls ~/.xmake/packages/l/levilamina/                # installed versions; xmake.lua pins the line (26.60.*)
L=$(ls -d ~/.xmake/packages/l/levilamina/26.60.*/*/include | tail -n 1)
grep -rn "class AnimateEntityPacket\b" $L/mc       # find the header that defines a type
grep -rln "struct ActorEquipmentComponent" $L/mc   # components live under mc/entity/components/
```

On Windows the packages are under `%LOCALAPPDATA%\.xmake\packages`. `compile_commands.json` lists the exact include
directory in its `-I` flags.

What the headers tell you:

- **Fields are `::ll::TypedStorage<Align, Size, T>`.** For a class type `T` the field is a wrapper, not a `T`: read it
  with `*packet.mController`, `packet.mController->size()` or `packet.mController.get()`, and assign through it
  (`packet.mController = std::string{...}`). For scalars, enums, references and `std::unique_ptr`/`shared_ptr`/
  `weak_ptr`/`gsl::not_null` it *is* the plain type, so `animate.mBlendOutTime` is a `float` and `*` does not apply.
  `ll/api/base/Alias.h` defines both cases.
- **`::ll::UntypedStorage<Align, Size> mUnk...`** fields have no known type. Do not use them.
- **`MCNAPI` functions are not available**: they are declared, deprecated and not exported, so they fail to link. Only
  `MCAPI`, `MCFOLD` and inline/virtual members can be called. `$name` functions are the thunks of virtual functions;
  call the virtual one.
- **Members that are not in the header do not exist.** If a class has no member for what you need, look for a component
  or a free function, or ask; do not invent one.
- **ECS components** (`mc/entity/components/*Component.h`) hang off an actor's entity, not the actor:
  `actor.getEntityContext().tryGetComponent<T>()` returns an `optional_ref`; `.as_ptr()` gives `T*` or null.
  See `src/playback/editor/input/FlySpeed.cpp`.
- **Forward declarations make incomplete types.** Headers declare most types they mention (`class Level;`). Include
  the type's own header (`mc/world/level/Level.h`) in the `.cpp` that uses its members; the error is "incomplete type"
  or "member access into incomplete type".
- **Packets**: a packet class `FooPacket` usually keeps its fields in `FooPacketPayload` (see `AnimateEntityPacket`).
  Create one with `MinecraftPackets::createPacket(MinecraftPacketIds::Foo)` and cast it.
- Existing Playback code is the best example of a working call: `grep -rn "<member>" src` before writing a new one.

## Unit tests

`xmake test` builds and runs `playback_tests` (doctest, `tests/*.cpp`), which links no game code: only headers and
sources that do not include LeviLamina. CI runs it on every pull request. To test a fix, move the logic into a function
that takes plain types (see `src/playback/record/ControllerTransitions.h` or `src/playback/state/editing/CameraKeys.h`),
call it from the game code, and add a test for the case that was broken. If the function lives in a `.cpp`, add that file
to `playback_tests` in `xmake.lua`. On Linux the test binary runs through Wine.

## Runtime checks

A build proves the code compiles, not that the mod works. Behaviour changes need a check in the game (see
CONTRIBUTING.md): copy `bin/playback/` into the game's `mods/playback/`, start the game, open a replay.
