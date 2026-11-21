# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Fly speed for the free camera: while the left mouse button is held over the viewport (flying with WASD), the mouse wheel raises (up) or lowers (down) the flight speed in 1.25x steps from 0.05x to 18x, shown in a **Fly speed** pill at the bottom of the viewport. The wheel no longer scrolls the hotbar while flying. The speed lasts until the replay closes.
- Layer locks: every timeline row (camera tracks, layers and the replay) has a lock toggle beside its eye. A locked camera track takes no keys, a locked layer cannot be moved, trimmed, edited or deleted, and a locked replay layer cannot be cut or trimmed. The control API takes `SetWorldActorLocked` for the replay layer.
- Cutting the replay: the timeline's **Replay** lane shows the replay's segments. The scissors on its row (or right-click > **Cut the replay at the playhead** / **Cut here**) split it, dragging a segment's edge trims it, and **Cut Out Segment** (right-click, the inspector, or Delete) removes one. Playback jumps over a cut and carries on, and exports leave it out of the video (the game clock runs on the video's time, so effects do not leap). The control API takes `DeleteWorldActorSegment`, and `GET /state` lists the `segments`.
- Title, shape and model layers over the replay: **+ > Add title**, **Add shape** (rectangle, circle, triangle or polygon) and **Add Mine-imator model...** put a five-second layer at the playhead with a lane of its own; drag it to move it, drag an edge to trim it, and right-click for duplicate, bring forward, send backward, trim to playhead, lock and delete. The inspector sets its timing and fades, opacity and visibility; a title's text, font (any installed font, searchable), size, bold, italic, alignment, colour and outline width and colour; a shape's kind, points (3 is a triangle, more approach a circle), fill, outline and corner radius; position, rotation, **Skew 2D** (slant) and **Skew 3D** (tilt in perspective). A model (`.mimodel`, textures beside it) stands in the world at a position, yaw, pitch, roll and scale, and **Place in front of the camera** moves it into view; it is drawn over the picture, so blocks never hide it. Layers are drawn by one renderer for the preview and every export, so the export matches the viewport. Saved in the project; the control API takes `SetOverlays` (`overlays`) and `GET /state` lists them.
- Blur layers: **+ > Add blur** puts a Gaussian blur, Mosaic (pixelate), Motion blur or Radial (zoom) blur on the timeline at the playhead, a lane of its own like any layer (move, trim, fade, lock, duplicate). It blurs the replay picture itself, under every title, shape and model, in the preview and in exports at any size, since its lengths are fractions of the frame's height. The inspector sets the style and its amount (radius, block size, streak length and direction, or zoom strength) and where it acts: the whole frame, or a rectangle or ellipse placed by position, size and rotation with a soft edge, which **Blur outside the area** turns around to keep a subject sharp. Amount, direction, soft edge, opacity and the area key like every other number. The GPU pass needs a graphics card that can write the frame's format from a compute shader; without it the blur is left out and the log says so.
- Light layers: **+ > Add light** puts a **Point light** (a glow of the layer's width and height), **Spotlight** (a cone from its position towards its rotation, with a spot angle, reach and source glow), **Strip light** (a band of light along the rotation, as through a window) or **Ambient light** (the whole frame) on the timeline at the playhead. It lights the replay picture under every title, shape and model, in the preview and in exports: **Level** brightens what it shines on (100% doubles it; negative darkens), **Hardness** runs from a soft falloff to a hard edge, **Colour temperature** sets the light from candle orange (1000 K) through white (6500 K) to blue sky (12000 K), and **Filter** puts a gel colour over it. Position, rotation, size, level, hardness, temperature, filter, spot angle and opacity all key.
- Minecraft blocks and items as layers: **+ > Add Minecraft block or item...** opens a searchable list of every block and item the game ships, read from its own resource packs (all vanilla version packs layered as the game layers them, including the packed `.brarchive` files). A block stands in the world as a full cube with its own top, bottom and side textures (the inventory look, with grass and leaves in plains colours); an item as its sprite one pixel thick, as a dropped item. It is a model layer: position, rotation and scale key, **Place in front of the camera** works, and **Choose block or item...** in its inspector swaps it for another (or **Browse...** for a Mine-imator file). Blocks with shapes other than a cube (flowers, slabs, stairs) show as cubes of their texture.
- **Stand-ins**: one player can fill a scene with many. The **Stand-in** button on the inspector's rail opens the hiring page and brings up the Replay layer with an **In** and **Out** flag on it: pick the player to copy, drag the flags (or set them at the playhead) around the stretch of the recording to take, and click **Hire Stand-in**. Their movement, turning, swings, what they hold and wear and their animations are extracted from the recording and become a Stand-in layer on the timeline, which moves and trims like any other (moving it changes when it plays; trimming it changes which part of the recording plays). Its inspector sets how it looks (the player's own skin, another recorded player's skin, a skin image with classic or slim arms, or any mob such as `minecraft:zombie`), a name tag, where it plays (moved in blocks and turned about where the player stood, both keyable) and **Repeat the blocks they changed**, which places and breaks the blocks the player changed within reach at the Stand-in's time and place; seeking back puts the world back.
- **Rule of thirds grid**: the `#` button in the viewport toolbar lays a thirds guide over the picture. It is never exported.
- **Export Still Frame (PNG)**: the camera button in the viewport toolbar (or **File > Export Still Frame**) renders the frame under the playhead at the export dialog's size, quality, folder and shader as `<time>-frame-<tick>.png`, layers included.
- Friendlier inspector: on/off settings (HUD, audio, World overrides, layer options) are flat rows with an icon and a switch, clickable anywhere along the row, and sections carry icons. Every number in the inspector can be dragged with the mouse: drag left or right on the field (Shift faster, Alt finer), double-click or Ctrl+click to type; a bounded value shows how far along its range it sits. Values that used to commit on typing (key tick, position, FOV and shake, marker and pose ticks, segment start and end) commit when the drag ends.
- The Replays screen has an open-folder button beside **Import**: it opens the replays folder (`mods/playback/data/replays`) in File Explorer, creating it if it does not exist yet. The control API takes it as the `OpenReplaysFolder` action.
- Each camera track row has a save-keyframe button beside its eye. It writes the current camera into the track's key at the playhead, keeping that key's interpolation and easing, or adds the key when there is none. On a first- or third-person track the key also takes the player the view follows, when it follows one. Locked tracks grey it out. One undo step.
- Export motion blur: **Video > Motion blur** renders each output frame 4, 8 or 16 times spread over the open shutter (**Shutter** 90°, 180°, 270° or 360°) and averages them, so movement smears like a film camera. The preview stays sharp. Each sample is a full render. The control API's `exportSettings` takes `motionBlurSamples` and `motionBlurShutter`.
- The viewport's frame shape is a true crop of the game view: at 9:16 the preview shows the centre of the frame at its real proportions instead of squeezing the whole window into it, and camera paths and entity picking follow the crop. Export's resolution presets follow the frame shape (1080p is 1080x1920 at 9:16). **Video > Same as project** (on by default) keeps the output at the frame shape: the height follows the width, and an output of another shape is moved onto it when the dialog opens. Turned off, any width and height can be typed.
- Viewport shading, as in a 3D package: three buttons at the right of the viewport toolbar switch the preview between **Flat** (the textures' own colours with no lighting, face shading, ambient occlusion, shadows or shader effects: the lightest to edit in), **Lit** (the project's graphics with the game's lighting and shadows, no shader pack) and **Rendered** (the shader pack picked under the arrow beside them, or vanilla). Flat runs Playback's own `Playback Flat` pack on Arke, written into Arke's `shaderpacks` folder when a replay opens and left out of the Rendered list; without Arke it falls back to Simple graphics at full brightness. Exports always render with the project's graphics and the Rendered pack. Packs are Arke's; Playback switches Arke's own setting while a replay is open and puts it back afterwards. The choice is saved with the editor layout.
- The World section overrides the weather: **Override weather** holds the replay world at Clear, Rain or Thunderstorm in preview and export, whatever the recording had. Rain falls where the biome has rain or snow, and a thunderstorm also darkens the sky; recorded lightning bolts still strike. Turned off, the recorded weather comes back. The control API's `SetWorldSettings` takes `overrideWeather` and `weather` (`Clear`, `Rain`, `Thunder`).
- The World section overrides the field of view: **Override field of view** (10° to 150°) gives every view the same FOV in preview and export: camera keys, the followed player and the free camera. The first-person hand keeps the game's own. The control API's `SetWorldSettings` takes `overrideFov` and `fov`.
- Shader options in the inspector's Preview page: the Rendered pack's own options (every option it declares, laid out by its own screens with its names and tooltips) are set per project and in place while the replay is open and in every export. Your Arke settings come back when the replay closes. Options the project sets are marked `*`; right-click one, or **Use my Arke settings**, to hand them back. The control API's `SetWorldSettings` takes `shaderOptions` as `{"<pack>": {"NAME": "value"}}` (null drops a value).
- Shader templates in the Preview page's Shader options: **Create template** saves the pack's options as they are in effect (your Arke values with the project's over them) under a name, and **Select template** applies a saved one to the project in one undo step; values equal to your own Arke settings are left to them. Templates are kept per pack as `mods/playback/data/shader_templates/<pack>/<name>.txt`, in Arke's own `NAME=value` options format.
- The fog override reaches shader packs and has a **Custom fog colour**: in Rendered and exports the fog start, end and colour go to the pack through its `FOG_START`, `FOG_CUSTOM`, `FOG_END` and `FOG_COLOR_R/G/B` options (each value snapped to one the pack lists), so the override no longer does nothing there. A pack without them keeps its own fog; the RedEagle Toon pack (`ToonShader_v1.3.1_Cinematic`) declares them. Lit and Flat keep the world's fog colour. The control API's `SetWorldSettings` takes `overrideFogColor` and `fogColor` (`[r, g, b]`, 0-1).
- Person camera tracks have a **Player** list in the track's inspector: it shows who the view follows at the playhead, and picking another switches to them from the playhead on (a key is added there when there is none).
- Keyframed camera shake on camera tracks: the track's **Lens & Shake** section sets the shake's strength, frequency and how far it moves (position in blocks, rotation and roll in degrees) and its seed. The noise is smooth and seeded on replay time, so preview and export shake the same way every time. The shake keys as one: the diamond beside **Camera Shake**, or a change to any of its values, saves all five in one key at the playhead, and the timeline shows them as one **Camera shake** row whose keys drag, ease and delete together.
- Keyframes per property, as in CapCut: every number of a camera track's **Lens & Shake** (field of view, and the shake as one), of a layer (opacity, position X and Y, size, rotation, skew, 3D tilt, font size, outline, corner radius, a model's position, yaw, pitch, roll and scale) and of the Preview page's World section (time of day, fog start and end, fog colour, override FOV, exposure, temperature and tint) keys on its own. Every change to one of them, by dragging, typing or the colour picker, is saved as a key at the playhead, and editing one property never adds keys to another. Each has a diamond beside its name (filled where it has a key at the playhead; click to add or remove one) with arrows to its previous and next key. Right-click a filled diamond for its easing (linear, ease in and out, ease in, ease out or hold). On the timeline, the chevron on a row with keyed properties expands it into one row per property showing its value as a curve with its keys: drag a key to move it, right-click it for easing, go to key and delete, and use the row's ◀ ◆ ▶ buttons as in the inspector. Collapsed rows mark their property keys along their foot, and a **World** row appears once anything in the World section is keyed. A layer's keys move with it; trimming its start leaves them where they are. Camera keys now hold only the camera's position and rotation: projects with a field of view or shake on their keys open with them as keyed properties (a single value becomes the track's value). The control API's camera tracks list `fov`, `shakeStrength`, `shakeFrequency` and `channels`, and take `SetCameraProperties` (`cameraProperties`) in place of `SetKeyframeFov`, `SetKeyframeShake` and `SetCameraShake`; layers and `SetWorldSettings` take `channels` too.
- Pose keyframes: right-click a player or mob in the viewport and pick **Add pose keyframe**, or use the timeline's **+ > Pose keyframe at playhead**, and the entity gets a lane on the timeline. From each key the entity plays its pose (as `/playanimation` would, on a controller of its own so recorded animations keep playing) until its next key; a **Back to in-game animation** key (a square) hands it back to the recording. Keys drag along the lane, right-click the lane to add one, and the inspector sets the entity, the pose (**Lean, arms crossed**, bundled in Playback's resource pack, or any animation a loaded pack defines), **Empty hands** (hides what it holds until the pose ends) and the tick. Saved in the project; shown in preview and export. The control API takes `SetPoseKeys` (`poseKeys`: `id`, `subject`, `subjectLabel`, `tick`, `animation`, `hideItems`), and `GET /state` lists them under `project.poseKeys`.

### Fixed

- Letting go of a World section slider could crash the game. The finished drag was committed from the draft after the draft had already been cleared, so the settings were copied from freed memory; with keyed World values (whose keys live in that memory) it crashed every time. Follow sliders had the same fault.
- The timeline's track list controls line up: the add and snap buttons are as tall as the search field beside them, and each row's buttons sit in even, square slots with a hover highlight instead of oversized icons touching each other. Long track names stop short of the buttons.
- A player riding when recording starts sits on its vehicle in the replay. The snapshot's spawn packet for the vehicle (a bicycle, a horse, a boat) named its rider by the recording player's live ID, which the replay does not know. Vehicles in the snapshot and vehicles spawned while recording now name the recording player as the replay knows it. Older replays are fixed by `repair_links.py` in the repair kit, which writes a `-ride` copy.
- Riders stay on add-on vehicles. The replay world loads no behavior packs, so the game refused a rider on an add-on bicycle or car, and each recorded movement teleported the rider back off: the mascot walked behind its bicycle. Recorded rides are now forced, riders are moved with their vehicle instead of teleported, and a rider keeps the seat it took when it got on (the recording player's own position trails its vehicle by about three ticks).
- Add-on blocks draw their per-state shape in replays: geometry, textures, placement-direction turn and light from the definition's permutations (a blank base with the wood and colour in permutations, a chain lamp, a facing). They used to be applied after the game had already baked each permutation's render schematic from the base, so every block drew its base look: chain lamps stacked into whole lamps, table fans and wrenches lost their body, folded ladders vanished and switches faced the base direction. They are now put on each permutation before the game initializes the block, with the base shape held back from initialization. Each permutation's condition is evaluated against its states (`q.block_state` comparisons with `&&`, `||`, `!` and arithmetic); a condition outside that subset leaves the permutation at its base look.
- Add-on blocks changed during a recording no longer vanish. The replay registered recorded add-on blocks in the local server's block palette but not in the client's, where block updates land, so a switch flipped or a lamp turned on resolved to air by its network ID.
- Vibrant Visuals stays on with Playback installed. LeviLamina loads Playback's own UI resource pack into every world, and the pack did not declare the `pbr` capability, so the game counted the world's packs as unsupported and fell back to Fancy, whatever Mode the settings had. The pack now declares it.
- Add-on blocks with states show in replays. A replay registered each recorded add-on block without the states and traits its definition declares, so it had a single permutation: blocks recorded in any other state (a colour, a facing, a cabinet role) drew nothing or showed as unknown blocks. Furniture such as cabinets, TV cabinets and standing lamps went missing. The states and placement traits are registered now, so every recorded block resolves to its own permutation.
- Seeking back no longer leaves blocks placed later in the world (a bridge built during the replay stood complete at its start). Re-sent snapshot columns did not replace blocks the client had already changed, so the replay now notes what each recorded block change replaced and puts those blocks back, newest first, when a seek returns to an earlier snapshot.
- Every player in a recording is shown in its replay. The recorder's start snapshot wrote the other players with an empty UUID: they collided on playback, so only one of them appeared and the player list showed duplicates. The snapshot now takes each player's UUID from the player list, and replays recorded before this fix are repaired as they open.
- Closing a replay gives Arke the user's own shader setting back. It was only put back while the editor drew, which it no longer does once the replay closes, so Arke stayed as the replay left it (often off, also after a restart). Playback also took a switch of Arke in its own settings between two replays for a pack already running, leaving **Rendered** and exports without the shader. With no pack picked under the arrow, Rendered now uses the pack Arke names even while Arke is off, rather than vanilla.
- Exports with a shader pack no longer open with seconds of dark, smeared frames. The export resizes the game view, which restarts Arke's pipeline from black, and the warm-up's frames passed in under a second, before its exposure and temporal buffers had settled. The warm-up now also lasts at least 4 seconds of real time. The viewport also shows the export's look (project graphics, Rendered pack) from the moment the export dialog opens, so a pack switched on from the Flat or Lit view has started too.
- Exports larger than the game window in either direction (1080x1920 on a 1920x1200 window) failed with "The captured frame could not be normalized to the export resolution", and an output of another shape than the window was stretched. The centre of the captured frame at the output's shape is now kept and averaged down or scaled up (bilinear) to the output size.
- Cinematic actors that keep their model on an anchor (RE Cinema scenes) no longer pop and jump in the preview while the replay plays. The replay poses now hold for the whole graphics update, as they do in exports, so the animations that place those models read the same position the models are drawn at.
- After a seek, mobs animated by `/playanimation` or scripts (RE Cinema scenes) show the pose the recording had at that tick instead of restarting their clip from the start or standing in their rest pose. The paused landing runs no replay ticks, and an actor's renderer only takes clips after its first frame, so the clips the seek's catch-up started are now sent again each frame until the actor holds them, then moved on to the time they had played. Flat shading and the project's graphics also follow the editor while paused.
- Animation controllers that add-ons build with `/playanimation` before a recording starts play in its replay. Such a controller is often sent once, when an item is equipped, and then driven by Molang variables, so a recording started later held only the variable changes and the player stayed in the default pose (Ore Fusion's Mjolnir flight showed the player standing). The recorder keeps every player's and mob's controller transitions while idle too and writes them into each snapshot after the actor. Replays recorded before this cannot be fixed this way: they never contained the transitions.
- Replays use the resource packs they carry even when a pack with the same identity is installed on the viewing machine, such as a development copy of the server's add-on. The installed pack used to win, and the replay drew with its version: Ore Fusion's Mjolnir animations from an older development copy were missing, so the client refused the controller states that used them and the player stood still instead of spinning the hammer or flying. An unencrypted embedded pack that collides now takes a uuid derived from its own while the replay is open, and the replayed pack list and stack name it.
- Animation controller transitions sent to an actor before its animation component is ready wait until it is, then go in together in their original order. A seek re-adds the recorded actors and their transitions in the same frame, and the client dropped them, so add-on poses stopped after a seek.
- From upstream: recorded player positions and replay player lists no longer drop players during respawn or when another entity reuses their runtime ID; piston animations stay aligned when camera tracks or cuts move the view across long distances; first-person views no longer jitter or clip into nearby blocks from integrated-server movement corrections; following another player in first person lowers the camera while crawling or gliding and hides entities riding them.

### Known issues

- After a seek, an RE Cinema scene's player actor (`re_cinema:<scene>_player`) can show unanimated, drawn upside down or misplaced and without its armor, until the replay plays into its next clip. Its animation component stays empty after the seek, so the resent clip never takes; mobs are not affected, and playing through and exports are. Workaround: seek a little before the shot and play into it.

## [0.4.0-mc26.60-re.4] - 2026-11-17

RedEagleStudios fork release, built from `re-main`.

### Fixed

- Burning entities in replays show their fire on their bodies instead of floating above or beside them. Replay entities' bounding boxes stayed where they spawned or last teleported, and the fire overlay, culling and viewport picking read them; they now follow the drawn pose.

## [0.4.0-mc26.60-re.3] - 2026-11-17

RedEagleStudios fork release, built from `re-main`.

### Added

- `POST /select` on the control API selects a keyframe as clicking it would, so the inspector shows it: `{"cameraId": "...", "tick": 120}`. `StartExport` takes `exportSettings` (`outputName`, `outputDirectory`, `startTick`, `endTick`, `fps`, `format`, `resolution`, `ssaa`, `warmupFrames`, `extraRenders`, `audio`, `hideHud`), so scripts can export a range.
- A Cubic Bezier keyframe's inspector shows its easing as a graph of progress over time to the next key. The two control points are dragged on the graph, a dot loops along the curve to show the motion, and each drag is one undo step. Time stays between the keys; progress may overshoot them. The Easing dropdown also offers None, which sets the keys back to Smooth.
- The World section has colour grading: exposure in stops, temperature from cool to warm and tint from green to magenta, applied in linear light to the preview and to exported frames. Temperature and tint keep the brightness. Saved in the project; the control API's `SetWorldSettings` takes `overrideColor`, `exposure`, `temperature` and `tint`.

## [0.4.0-mc26.60-re.2] - 2026-11-17

RedEagleStudios fork release, built from `re-main`.

### Added

- A local HTTP API on `127.0.0.1:7878` drives the replay editor like its UI, so scripts and tools can control a replay without the mouse or keyboard: `GET /state` returns the editor state as JSON (playhead, tracks and keys, world and follow settings, players, replay list, export), `GET /actions` lists the action types, and `POST /action` runs one, e.g. `{"type": "Seek", "tick": 1700}`. It listens on the loopback address only and is on by default (`control.enabled`, `control.port`).
- The World section sets the replay's graphics mode (Simple, Fancy or Vibrant Visuals) and its global resource packs. The chosen packs, in the listed order, replace the viewer's own global packs from when the replay world loads until it closes; the replay world's packs stay above them, and changes take effect the next time the replay opens. The graphics mode follows the project while the replay plays. Both are saved in the project and apply to preview and export, and the viewer's own settings come back when the replay closes. The control API's `SetWorldSettings` takes the same fields, and `GET /state` lists the installed packs.

### Fixed

- Opening a replay from a server with add-on blocks no longer crashes the game while the replay world loads. Recorded custom blocks are initialized one at a time after registration, before their component storage is finalized; a block the game cannot initialize is skipped and named in the log instead of crashing the game.

## [0.4.0-mc26.60-re.1] - 2026-11-17

RedEagleStudios fork release, built from `re-main`.

### Added

- Camera views set by the server (`/camera` and add-on cutscenes, including spline cameras) are recorded once per tick and become one keyframe track per cutscene, named Recorded camera, when the replay opens. Keys are kept only where the motion bends, and cuts hold the pose before them. The tracks can be previewed, disabled, edited or deleted like any other; deleting them is remembered in the project. Replays recorded with these views no longer let the server's camera take over the free camera; fades still play.
- The inspector's Preview page has a World section saved in the project and applied to preview and export: a time of day (frozen, or running from the set time), hiding the clouds, and a fog override whose start and end are set in percent of the render distance.
- Third-person camera tracks have a Follow section: behind (distance and height), over the shoulder, orbit (speed and start angle) and tripod (a fixed position taken from the current view), with optional smoothing. Preview and export frame the same way.
- Keyframes have easing presets (sine, quad, cubic, quart, quint, expo, circ and back, each in, out and in-out) in the inspector and the keyframe menu. Back overshoots the keys.

### Fixed

- Add-on entities whose models read client-synced entity properties are drawn in replays. Only the last entity type's property definitions were recorded, so cutscene actors were invisible.
- Seeking into a running `/playanimation` clip resumes it at the recorded time instead of restarting it, so cutscene actors are posed correctly after a seek.
- The free camera stays movable when the recording contains input locks or movement corrections sent to the recording player.
- Recorded custom skins are shown instead of Steve or Alex, and `/playanimation` on the recording player plays in replays.
- Runs under Wine and Proton: the editor falls back to a window swapchain when composition swapchains are unavailable, and the game image check accepts the GDK layout.

### Changed

- The SHA3-512 hashes of cached chunks are computed with mbedtls instead of OpenSSL, with identical results.
- `tooth.json` points at `github.com/RedEagleStudios/Playback`.

## [0.4.0-mc26.60] - 2026-11-15

### Added

- Added first-person follow for the recorded player, with teleport and follow actions in the camera menu. The view uses the recorded camera offset and rendered FOV (including the spyglass), the recorded HUD (inventory, armor, offhand, selected slot, health, hunger, experience and game mode), held maps, view bobbing, hand sway, the nether portal warp and the bed view with its fade to black while sleeping. Recorded container screens are previewed while following.
- Other recorded players and any entity can be followed in first person, from the subject picker or the viewport's right-click and nearby entities menus. When the subject leaves the server, or an entity stays gone, the view returns to the recorder.
- Video export can render first-person video from the chosen subject, with the subject's arm and held item and without its model.
- Video export now captures optional mono or stereo game audio: AAC, Opus or FLAC in MP4, or an uncompressed sidecar `.audio.wav` for PNG sequences. AAC and Opus offer 128, 192, 256 or 320 kbps; AAC is the default. The game's sound output is restored when the export ends.
- Recordings now embed the server's downloaded resource packs and mount them on playback, so server textures and models show anywhere. Extracted packs are cached for the three most recent replays. `record.embedServerPacks` in the config turns this off.
- The local player's view can be recorded between ticks with `/record rate` (20 to 120 samples per second; 20 records once per tick, the default). Replay uses those samples instead of a straight line between ticks. The starting rate comes from `record.localPlayerUpdatesPerSecond` in the config; `/record rate` changes it until the game restarts.
- Camera keyframes are now applied to the preview while the replay is paused.
- Added first-person and third-person camera tracks next to keyframe tracks. A person track views a replay player from its first key to its last, and each key picks the player from the inspector's searchable list. Where tracks overlap, the highest track in the list plays, in preview and in export. The viewport camera chip names the track that is playing.
- Added a searchable player list for choosing which player to follow in the viewport.
- Added right-click menus for keyframes, empty lanes, track headers and the ruler on the timeline.
- Tapping Shift on its own while following a player now stops following.
- Added a third-person follow view, toggled with F5 while following a player. A sleeping subject still shows its first-person bed view.
- Camera tracks can be renamed, locked and cleared from their right-click menu, and a keyframe's menu can move the free camera to its pose.
- Added multi-select for keyframes: Ctrl+click, box select on empty canvas and Ctrl+A. Selected keyframes move, delete, copy, paste and change interpolation together.
- Holding Shift while dragging snaps to keyframes, markers, segment edges, the playhead and the export range.
- Markers can be added with M or from the ruler menu, dragged, renamed and deleted. `[` and `]` jump between them.
- Added recording controls to the small icon row of the pause menu: start or resume, pause, finish and cancel.
- Finishing a recording shows a save dialog with a name input that defaults to the timestamp, plus save and delete actions. Deleting and cancelling ask for confirmation first. Escape cannot dismiss the save dialog, so a finished recording cannot be dropped by accident. Starting a new recording while one is still waiting for a name saves it under its timestamp name first.
- Added a top-right recording indicator that shows the recording state and elapsed time. `/record indicator [on|off]` shows or hides it. It never appears in replays, the editor, exported videos or recording thumbnails.
- The inspector has a Preview page for choosing which HUD elements a replay shows: chat, boss bar, title text, scoreboard, action bar and hotbar. Only the hotbar is on by default; the choices are saved and also apply to exported video. The export dialog's Hide HUD option leaves the whole HUD out of an export.
- The inspector's Preview page has audio controls for the editor preview: mute, volume, and Silent while paused, which keeps sounds triggered while the replay is paused or seeking from playing. They are saved and do not affect exported audio.

### Changed

- The camera path overlay and the paused camera glyph only show for keyframe tracks.
- The camera switch track was removed, and its data in earlier editor projects is dropped.
- Replays recorded before `v0.4.0` are no longer supported and must be recorded again. Older archives may not be detected as incompatible; back up recordings and projects before upgrading. No migration is provided.
- Dropdowns, popups and context menus in the editor now open under the control that opened them, with menu padding.
- Recording snapshots reuse unchanged chunk columns and serialize changed columns on background threads to reduce game stalls.
- Seeking in a replay now catches up faster by loading the world in merged chunk batches.
- Requires exactly Minecraft `1.26.60.5`; Playback refuses to load on other game builds and reports the required version.
- Exports now always run a client tick as soon as they ask for one instead of waiting for the game's 20 Hz timer, which the experimental Accelerate client option used to turn on. The option is removed.
- Exports on DirectX 12 now draw a frame once instead of two or more times, which shortens them; a frame is only drawn again while the replay is still preparing the next one. Ray-traced lighting therefore adapts about once per exported frame, so how quickly it follows a change such as the time of day no longer depends on how fast the export happens to run.
- The export setting Convergence frames is now called Extra renders, which is what it does: each frame is rendered that many more times before it is saved. What it does and its default of 0 are unchanged.
- The export dialog now has General, Video, Audio and Advanced pages. Export buttons stay visible on every page; the summary includes encoder settings and hides first on short windows before the settings page scrolls.

### Fixed

- Recordings remain playable when the server switches dimensions again before the new world finishes loading. Stopping during that transition keeps the portion already recorded.
- Terrain stays visible in continuous previews and video exports after rapid dimension changes.
- Replays from servers with custom items now record the server's item registry and apply it on playback, so recorded item ids resolve to the server's items.
- Fixed a freeze when chunk draining re-entered the level join while recording.
- Particles are rebuilt after a seek.
- Recorded container screens stay open across seeks and exports, and the inventory screen is no longer previewed.
- Fixed a dimension loading screen that could stay up forever during export.
- First-person follow now shows the followed player's arm swing and item-use animation, such as eating.
- Recordings now keep the recording player's own predicted sounds, such as hitting blocks, landing and strong attacks, so replays and exported audio no longer miss them.
- Recordings now keep the sounds the client plays for every entity itself, such as footsteps, jumping, mob calls and wing flaps, and replays mute the copies the game regenerates, so replays and exported audio match what was heard and nothing plays twice.
- Fixed item pickups, mob hurt and death sounds, and other entity and block events playing twice in replays. Each packet the client handles is now recorded once, as it was received.
- Fixed a crash when quitting the game after the replay editor had been opened or while a video export was running.
- Fixed the free camera's FOV widening while a followed first-person player sprinted. Following a player now only copies the attributes the HUD shows.
- Recorded server forms no longer trap the free camera. Containers and forms are previewed only while following the recorder in first person; seeks restore their recorded state and switching away closes the preview. Other server response screens are not opened.
- Ray tracing and Vibrant Visuals exports now time auto exposure and other time-driven effects on the replay instead of on how long each frame took to render, so exported brightness adapts at the same pace as in the replay.
- Exports now repeat the renderer warm-up after the replay changes dimension, so the first frames in the new dimension are no longer captured while auto exposure is still adapting.
- Ray tracing exports of the same replay no longer differ in brightness for a few seconds after the lighting changes, such as after a time change. The game now draws on every frame it sends to the renderer during an export.
- Exports now time interface animations, such as a chat message leaving the HUD, on the replay instead of on how long the export took, so a message stays on screen for the same length of video in every export.
- Exports now run particles, the held item's equip animation, the sky, cloud and rain transitions and HUD timers such as the item name on the replay instead of on how long the export took, so they move at the same speed in the video as in the replay.
- Recordings now take the snapshot of a new dimension once the player has finished loading it, including the short gap before the loading screen appears, so replays and exports show the terrain the player first saw right after a dimension change instead of an empty world that fills in over the next second or two. Replays recorded earlier are unchanged.
- Exports without a camera track keep their fixed 70 degree field of view after a dimension change instead of switching to the player's field of view setting for the rest of the video.
- Replays and exports of the End no longer show a second ender dragon and boss bar that the replay world spawned on its own, and no longer drop a recorded entity that shared its id.
- Replay worlds now use the recorded world's seed instead of choosing a new one.
- Replays and exports now preserve the recorded biomes when returning to a dimension, keeping the sky and water colors consistent with the recording.
- Closing the game from a replay now waits for its temporary world to close and be removed, including when an export is in progress.
- The replay list's selection summary separates the file count and the total size with a middle dot instead of a garbled character.
- Editor dialogs and menus stay inside the game window at any size. The keyboard shortcut sheet and message dialogs stay centred when the window is resized, the shortcut sheet switches to two columns or scrolls on a short window, menus too long for the window scroll, and long tooltips wrap. On a narrow window the replay list toolbar shrinks the search box first, then drops the import and sort labels and finally the title.
- Numbers typed into the export dialog take effect at once, without pressing Enter, so starting the export straight after typing uses them. Out-of-range values are clamped as they are typed.

## [0.3.3-mc26.60] - 2026-11-03

### Changed

- Adapted to LeviLamina 26.60.*

## [0.3.3-mc26.50] - 2026-10-11

### Added

- Added encode speed (Quality, Fast, Fastest) and CRF settings for MP4 export under Advanced. They map to the libx264 `medium`, `veryfast` and `ultrafast` presets and `-crf`; the defaults match the previous fixed `medium` / CRF 18.
- Added an experimental Accelerate client option under Advanced that hands out client ticks as soon as an export asks for one instead of waiting for the game's 20 Hz timer. It is off by default.

### Changed

- Regrouped the export dialog into output, timeline and capture sections plus a collapsible Advanced section that keeps the dialog height and scrolls instead of growing.
- Export frames whose scene misses the capture are now re-rendered as soon as the miss is detected instead of after a timeout, making exports several times faster.
- Splitters and the timeline zoom bar now show an open or closed hand cursor, and splitters highlight while hovered or dragged.
- Restyled the timeline zoom bar after Premiere Pro, with ring handles and a thin grey bar.
- Delete, rename and error prompts in the replay list and editor now share one dialog style.
- The selected replay in the replay list is tinted with the accent colour and outlined, so it is easy to tell from hover.
- Raised the export convergence frame limit from 240 to 1000.
- Moved the export convergence frame count into the Advanced section and made it default to 0 on every renderer, including ray tracing.
- Opening the Advanced section of the export dialog now scrolls it into view.

### Fixed

- Fixed long exports slowing down and GPU usage dropping after about 15 minutes without input. The game's Eco Mode is now switched off while an export runs and restored afterwards, including when the export fails with an error or the game is killed mid-export (restored on the next launch).
- Pinned the replay world time before every render pass of an export frame, so the level clock cannot drift between passes.
- Fixed the export progress UI blurring when the export resolution is below the window size; it now draws on its own layer at the window resolution.
- Fixed the replay list flashing and reloading every thumbnail when a replay is deleted.
- Fixed the selected replay's highlight flickering in the replay list.

## [0.3.2-mc26.50] - 2026-10-01

### Added

- Added a 4x supersampling tier to video export. Tiers whose supersampled render size does not fit are disabled for the chosen resolution.
- Added a convergence frame count to the export settings. Each frame is rendered without a capture until the ray-traced denoiser settles, removing green and yellow streaks from exported skies; it defaults to 8 with ray tracing active and 0 otherwise.
- Added support for the separate OptiPiston mod: when it is installed, Playback drives its piston animation from the replay clock, with its settings in the inspector.

### Changed

- Exports now use the chosen name verbatim as the output file name instead of rewriting it into a folder with a parameter suffix. Existing files get a numbered suffix, and the default name is a timestamp.
- Refined the export settings and progress dialogs: a fixed height that fits 1080p without a scrollbar, clearer hints and timecodes, and a translucent progress overlay with a pill-shaped cancel button.
- Supersampled captures are now downsampled on the GPU, so readback stays at the output size; frames are normalized and encoded in separate stages, and captured pixel buffers are reused.
- The replay browser now loads thumbnails on demand with bounded caches.
- Kept the configuration version, recording-file snapshot context version, and project format version at `1`; existing `v0.3.1` archives and projects remain compatible.

### Fixed

- Fixed player skins not loading in replays on MC 26.51. Skin-change packets are now recorded and remapped, snapshots fall back to the player's own skin, and player-list entries without a skin get the default skin.
- Fixed custom block geometry not being initialized in replays on MC 26.51.
- Kept the offline export clock in exact milliseconds on MC 26.51 so exported frame timing does not drift.
- Fixed the export cancel button not responding to the mouse while an export runs.
- Kept the renderer drawing while an export waits, so the render clock keeps advancing instead of stalling the export.
- Preserved completed offline frame samples so they are not discarded before capture.
- Hid camera paths and markers while the game HUD is covered by loading, progress, or menu screens, and restored them when the HUD returns.

## [0.3.1-mc26.50] - 2026-09-23

> **First release on the MC 26.51 line, branched from `v0.3.1-mc26.40`.**

### Added

- Added `Ctrl+Shift+N` to create a camera track and `1`, `2`, `3` to switch to the first three cameras, which also drives the viewport preview.
- Added `Ctrl+Shift+-`, `Ctrl+Shift+=`, and `Ctrl+Shift+0` to step and reset the interface scale.
- Added `Alt+Wheel` to zoom the timeline around the cursor.
- Added a keyboard shortcut reference to the getting-started guide.

### Changed

- Moved timeline zoom to `-` and `=`, zoom-to-fit to `\`, and playback speed to `J` and `L`, matching common video editors. `Shift+Wheel` no longer zooms.
- Made `K` add a keyframe to the first usable camera track when nothing is selected instead of doing nothing.

### Fixed

- Fixed recorded players standing still on MC 26.40 by restoring their entity-id registration and remapping the local-player snapshot's abilities and entity links.
- Fixed replay disconnections caused by teleport movement packets missing their required teleport data.
- Fixed backward seeks stalling and closing the replay when previously loaded request-mode chunk columns were not replaced with the recorded data.
- Preserved recorded player names instead of forcing synthetic names, and restored names from the recorded player list for affected existing recordings.
- Fixed entities jittering during preview playback. Recorded poses are now interpolated on preview frames as well as during export, and are sampled one tick behind the camera so the pose history is actually interpolated instead of snapping to its end.
- Fixed one missing or incomplete entity cancelling pose interpolation for the whole frame, and left passenger positions to their vehicle so riders no longer fight the mount.
- Fixed camera-path overlays being sampled across dimension boundaries, which spent work on points that were discarded at draw time.
- Fixed a plain click on a timeline keyframe nudging it; any horizontal movement is now required before a drag begins, so a click only seeks.
- Fixed players not loading in the replay when a server withheld skin data for any one of them. A single entry without a skin discarded the whole player list; that entry is now skipped and the rest of the list still spawns.
- Fixed the real local player's identity leaking into recorded `AddPlayer` packets through the abilities payload's target-player field, which is where MC 26.40 moved that field.
- Fixed the imgui SRV descriptor heap running out once the font atlas and per-replay browser thumbnails were added; the heap is now larger and logs instead of handing imgui a null descriptor when it is still exhausted.
- Fixed the editor UI waiting on full chunk streaming before it appeared; it now shows as soon as the loading and progress screens clear.
- Fixed stale replay-player references after a failed refresh, remapped `AddPlayer` abilities and link references from the real local player to the recorded sentinel id, and stopped raising the global runtime-id allocator to the synthetic recorded-player id.
- Fixed passenger detection after the `isRiding` overload it relied on was removed upstream.

## [0.3.1-mc26.20] - 2026-09-14

### Added

- Added a camera-path overlay in the viewport. Camera tracks are drawn as splines with keyframe markers, clipped against the near plane so the path stays anchored to the rendered view.
- Added selectable UI scale tiers for the editor, applied without re-rasterising glyphs.
- Added direct playback-speed selection in the transport, snapped to the same speed ladder the keyboard shortcuts step through.
- Added an export-range marker on the timeline, with `I` and `O` to set its bounds and `Space` to toggle play/pause.

### Changed

- Rebuilt the editor layout around a centred transport, with reworked timeline rows, details panel, status bar, and menu bar.
- Aligned timeline and toolbar icons optically rather than on their glyph boxes.
- Kept the configuration version, recording-file snapshot context version, and project format version at `1`; `v0.3.0-mc26.20` archives and projects remain compatible.

### Removed

- Removed the scene inspector page and the property search box; neither affected the replay or the export.
- Removed the world-actor segment speed slider, command, and action. Segment speed had no export path, so it never changed playback; the serialised field stays because splitting a segment derives its source tick from it.

### Fixed

- Fixed replays losing a dimension boundary when the recorder rotated the chunk instead of leaving a packet on the timeline; boundaries are now derived from the snapshot contexts.
- Fixed columns without complete subchunk coverage being injected, which could leave partially built terrain.
- Fixed clicks on viewport overlays reaching the game camera.

## [0.3.0-mc26.20] - 2026-09-08

### Added

- Added persistent editor projects. Editing state is saved to `<dataDir>/projects/<replay>.pbproj` on Ctrl+S, from the menu, every 30 seconds, and when the editor closes; the replay file remains authoritative for the timeline duration.
- Added replay of server custom blocks. Block types recorded in the StartGame packet are registered when the replay world loads and rendered with their recorded geometry and materials.
- Added resolution of custom dimensions by their recorded name, so replays can reconstruct dimensions whose ids are not vanilla.
- Added the recording game version to replay metadata. The replay browser refuses to open files recorded on a different Minecraft version and shows both versions.

### Changed

- Made keyboard input UI-exclusive while the editor is open unless the left mouse button is held over the game viewport. Escape no longer reaches the game; holding it for 0.8 s exits the editor and saves the project.
- Kept the editor hidden until the replay world is joined and ready, so loading, resource-pack download, and sign-in screens are no longer covered.
- Cut seek time roughly in half by draining chunk injection inline during catch-up and reusing already-applied snapshot columns.
- Downgraded project save/load, keyframe capture, and custom block registration logs to debug.
- Kept the configuration version, recording-file snapshot context version, and new project format version at `1`; complete `v0.2.1-mc26.20` archives remain compatible and require no conversion.
- Documented that Vibrant Visuals is not supported yet; recording, replay, and export are only validated with the standard renderer.
- Documented that `0.2.0` was skipped on the MC 26.20 line because video export stalled on that runtime, and that `0.2.1` is published for both runtimes.

### Fixed

- Fixed replays recorded by a newer build failing outright when a configuration packet had an unknown lifecycle; such packets are now skipped with a warning.
- Fixed Ctrl shortcuts such as undo/redo never triggering because ImGui modifier state was not submitted.
- Fixed the camera preview keeping a stale cached pose after a camera track was toggled.
- Fixed timeline wheel-zoom anchoring to the wrong tick when the cursor was outside the canvas.
- Fixed the property search box discarding typed text every frame.
- Fixed a rewind keeping stale player-list skins instead of re-registering them.

## [0.2.1-mc26.20] - 2026-08-27

### Changed

- Reworked export capture around the RenderDragon scene target and semantic world-submission detection instead of the swap-chain UI target and a draw-count threshold.
- Polished editor and replay-browser scrolling, timeline routing, selection feedback, icon alignment, compact controls, animations, and bilingual labels.
- Preserved the release tag during CI builds so packaged manifests carry the published version.
- Kept the configuration version and recording-file snapshot context version at `1`; complete `v0.2.0-mc26.20` archives remain compatible and require no conversion.

### Removed

- Removed unreachable legacy clip, track, transition, clipboard, menu, hint-bar, curve-editor, and entity-render-hook code.
- Removed periodic camera, entity-pose, and graphics-capture diagnostics from normal rendering and export paths.

### Fixed

- Fixed D3D12 exports capturing the UI composition target, which could produce uniform sky-color frames or include the export overlay instead of the world scene.
- Fixed sparse scenes stalling export when world submissions fell below an arbitrary draw-count threshold.
- Fixed the HBUI idle screen appearing in captures while Playback owned input.
- Fixed camera preview stepping backward when native render alpha and replay ticks advanced on different clocks, especially at reduced playback speeds.
- Fixed ordinary observer server synchronization abandoning a parked preview camera after pausing.
- Fixed replay-browser wheel scrolling and crowded timeline labels or click routing obscuring the intended target.

## [0.2.0-mc26.20] - 2026-08-20

### Added

- Added an in-game cinematic camera workflow with camera creation, capture-at-playhead, keyframe editing, sequence binding, live preview, property inspection, and undo/redo.
- Added Smooth, Linear, Ease In, Ease Out, Ease In/Out, Hold, Hermite, and Cubic Bezier camera interpolation.
- Added experimental frame-by-frame H.264 MP4 and PNG-sequence export with configurable tick range, frame rate, resolution, SSAA, warm-up, progress, cancellation, finalization, output validation, and clear-frame retry handling.
- Added D3D11 and D3D12 framebuffer capture paths. D3D12 supports SSAA 1x/2x; D3D11 uses the stable 1x path.
- Added camera-driven observer/chunk tracking and stable-frame warm-up after fast movement or dimension changes.

### Changed

- Flattened the source tree into recording, replay, editor, state, keyframe, exporting, visuals, runtime, packet, and I/O domains.
- Made every recorded dimension change an interpolation boundary. Camera curves never connect across a dimension transition, including when an intermediate dimension has no keyframes.
- Made paused replay use the freely movable observer camera, while active keyframe preview/export uses the sampled timeline camera.
- Reworked replay rendering so preview and export share the camera timeline, entity-pose sampling, observer synchronization, and cross-dimension state machine.
- Unified entity rendering around one sampled pose per frame and temporarily suppressed native movement interpolation during export rendering.
- Reworked chunk recording to preserve native request-mode packets for local worlds while converting inline or cache-enabled server chunks into portable replay data after client decoding.
- Changed export preparation to rely on replay tick, dimension transition, UI stability, and warm-up state instead of requiring every chunk in a fixed camera neighborhood.
- Derived the packaged mod version from the nearest Git release tag so hotfix builds retain the `0.2.0-mc26.20` identifier.
- Set the product version to `0.2.0` and the MC 26.20 release identifier to `v0.2.0-mc26.20`.
- Kept the configuration version in `Config.h` and the recording-file snapshot context version at `1`; no configuration or replay migration layer is provided.
- Replays recorded before this hotfix on affected servers may already be missing portable chunk or custom-entity registry data. Those archives cannot be repaired and must be recorded again; complete `v0.2.0-mc26.20` archives do not require conversion.

### Removed

- Removed compatibility branches for older action payload layouts. Replay archives created by previous Playback releases are incompatible and must be recorded again.

### Fixed

- Fixed preview and export stalls when entering, revisiting, or leaving dimensions.
- Fixed forced-snapshot reader boundaries being considered export-ready before the next replay chunk was applied.
- Fixed camera paths loading or capturing the wrong chunk region during fast movement and cross-dimension export.
- Fixed high-speed player ghosting caused by native and replay render paths observing different interpolated positions.
- Fixed free-camera translation stutter, pause rebound, third-person offsets, mouse grab/release loops, and unnecessary pause-time server teleports.
- Fixed raw camera FOV units, D3D11 supersampling fallback, D3D12 clear-frame recovery, and SSAA values above the stable 2x limit.
- Fixed replay terrain appearing as air on servers that send inline or cache-enabled chunk data.
- Fixed local-world seek failures caused by duplicate chunk columns or missing subchunk sections after server compatibility conversion.
- Fixed resource-pack handshakes discarding actor-registry configuration required to reconstruct custom server entities in newly recorded replays.
- Fixed snapshot application choosing an older LevelChunk record when multiple versions of the same chunk column were present.
- Fixed offline video export stopping when Minecraft lost focus or attempted to open pause or suspend UI.
- Fixed valid exports remaining at frame 0 because a fixed 5x5 camera-neighborhood readiness check could never be satisfied.

## [0.1.2-mc26.20] - 2026-08-03

### Added

- Added replay-browser search, playable/unavailable filters, five sorting modes, grid/details views, import, rename, multi-selection deletion, path copying, and File Explorer integration.
- Added replay thumbnail capture into `icon.png` and thumbnail display in the browser on both D3D11 and D3D12.
- Added in-memory editor groundwork for sequence and world-actor segment operations, camera-track data and keyframes, and undo/redo. Project persistence and video export remain unavailable.
- Added D3D11 rendering support for the native replay UI alongside the existing D3D12 path.

### Changed

- Changed the replay metadata and snapshot stream to require per-chunk dimension/view context, explicit local-player creation, and forced snapshot boundaries. Replays created by `v0.1.1` or earlier are not compatible and must be recorded again after upgrading.
- Kept the internal `Config` version field at its initial value (`1`) and left third-party dependency version declarations unchanged. No configuration or replay migration path is provided.
- Replaced the resource-pack-based replay browser with a native ImGui browser while retaining a lightweight resource-pack button on the main menu.
- Rebuilt the existing replay editor around resizable viewport, details, and timeline panels, zoomable tracks, viewport maximize/restore, and revised transport controls.
- Refactored replay snapshots to carry dimension context and local-player state explicitly, and to force snapshot playback at dimension boundaries.
- Unified command, replay-browser, and editor messages through the English and Simplified Chinese catalogs using LeviLamina's native translation path.
- Bundled the Lucide icon font directly with the mod and reduced the UI resource pack to its main-menu button, bilingual label, and required metadata.
- Consolidated the feature overview in the root README files and replaced the legacy documentation tree with focused installation and source-build guides.

### Fixed

- Restored installed and standalone `playback-ui.mcpack` packaging so the main-menu button is available in complete mod installations and as a separate Release asset.
- Reworked cross-dimension recording and replay handling to close the source chunk at the tick boundary, wait for the target dimension, and apply a dimension-scoped forced snapshot.
- Prevented ordinary forward seeks from reloading snapshots and moving the viewer; snapshot reloads remain for backward seeks, dimension changes, and forced boundaries.
- Improved replay snapshot cleanup and chunk application by refreshing the replay player, isolating chunks by dimension, filtering unsuccessful subchunk responses, and clearing recorded objectives between snapshots.
- Expanded D3D12 command-queue and swap-chain capture to cover additional renderer creation paths.

## [0.1.1-mc26.20] - 2026-07-29

### Added

- Added PDB symbols to release assets to help diagnose crashes.
- Adapted to Minecraft 26.20.4

## [0.1.0-alpha.2] - 2026-07-29

### Changed

- Bundled the Playback UI resource pack with the mod so LeviLauncher and Lip installations load it automatically through LeviLamina.
- Kept `playback-ui.mcpack` available as a standalone release asset for manual import.

  > **This release changes installation packaging only. The replay format and replay runtime behavior are unchanged from `0.1.0-alpha.1`.**

## [0.1.0-alpha.1] - 2026-07-27

### Added

- Client-side recording with asynchronous replay storage and export.
- Main-menu replay browser and isolated replay-world loading.
- Replay timeline controls for pause, seek, speed, and exit.
- Chunk snapshots, cached chunk replay, entity movement, and selected game-packet replay.
- English and Simplified Chinese localization for commands, the replay editor, and the resource-pack UI.

  > **This is the first public test release. Replay files and behavior may change before `1.0.0`.**
  > **Playback currently targets Windows x64 and the LeviLamina `26.10.*` client runtime.**

[Unreleased]: https://github.com/wo55555/Playback/compare/v0.4.0-mc26.60...HEAD
[0.4.0-mc26.60]: https://github.com/wo55555/Playback/compare/v0.3.3-mc26.60...v0.4.0-mc26.60
[0.3.3-mc26.60]: https://github.com/wo55555/Playback/compare/v0.3.3-mc26.50...v0.3.3-mc26.60
[0.3.3-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.2-mc26.50...v0.3.3-mc26.50
[0.3.2-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.1-mc26.50...v0.3.2-mc26.50
[0.3.1-mc26.50]: https://github.com/wo55555/Playback/compare/v0.3.1-mc26.20...v0.3.1-mc26.50
[0.3.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.3.0-mc26.20...v0.3.1-mc26.20
[0.3.0-mc26.20]: https://github.com/wo55555/Playback/compare/v0.2.1-mc26.20...v0.3.0-mc26.20
[0.2.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.2.0-mc26.20...v0.2.1-mc26.20
[0.2.0-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.2-mc26.20...v0.2.0-mc26.20
[0.1.2-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.1-mc26.20...v0.1.2-mc26.20
[0.1.1-mc26.20]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.2...v0.1.1-mc26.20
[0.1.0-alpha.2]: https://github.com/wo55555/Playback/compare/v0.1.0-alpha.1...v0.1.0-alpha.2
[0.1.0-alpha.1]: https://github.com/wo55555/Playback/releases/tag/v0.1.0-alpha.1
