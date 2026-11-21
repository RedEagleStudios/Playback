// Lucide icon font codepoints (assets/fonts/lucide.ttf, ISC license).
#pragma once

namespace Playback::ReplayEditor::Icons {

// Time icons
#define ICON_PLAY           "\ue13c" // play
#define ICON_PAUSE          "\ue12e" // pause
#define ICON_STOP           "\ue167" // square
#define ICON_RECORD         "\ue13c" // fallback: play (record not in editor)
#define ICON_SKIP_BACK      "\ue15f" // skip-back
#define ICON_SKIP_FORWARD   "\ue160" // skip-forward
#define ICON_CHEVRONS_LEFT  "\ue072" // chevrons-left
#define ICON_CHEVRONS_RIGHT "\ue073" // chevrons-right

// Marker / keyframe
#define ICON_ADD_KEYFRAME "\ue5e2" // diamond-plus
#define ICON_KEYFRAME     "\ue2d2" // diamond
#define ICON_ADD_MARKER   "\ue613" // map-pin-plus
#define ICON_MARKER       "\ue111" // map-pin

// Edit operations
#define ICON_SPLIT "\ue14e" // scissors
#define ICON_UNDO  "\ue2a1" // undo-2
#define ICON_REDO  "\ue2a0" // redo-2
#define ICON_CUT   "\ue14e" // scissors (fallback)
#define ICON_COPY  "\ue09e" // copy
#define ICON_PASTE "\ue09e" // copy (fallback)

// File
#define ICON_OPEN       "\ue247" // folder-open
#define ICON_SAVE       "\ue14d" // save
#define ICON_EXPORT     "\ue19e" // upload
#define ICON_FILE       "\ue0c0" // file
#define ICON_FILE_VIDEO "\ue321" // file-video

// Generic actions
#define ICON_ADD    "\ue13d" // plus
#define ICON_DELETE "\ue18e" // trash-2
#define ICON_TRASH  "\ue18e" // trash-2

// State toggles
#define ICON_LOCK    "\ue10b" // lock
#define ICON_UNLOCK  "\ue10c" // lock-open
#define ICON_MUTE    "\ue1ac" // volume-x
#define ICON_HIDE    "\ue0bb" // eye-off
#define ICON_EYE     "\ue0ba" // eye
#define ICON_VISIBLE "\ue0ba" // eye

// UI controls
#define ICON_DRAG         "\ue0eb" // grip-vertical
#define ICON_CHEVRON_DOWN "\ue06d" // chevron-down
#define ICON_CHECK        "\ue06c" // check
#define ICON_SEARCH       "\ue151" // search
#define ICON_RESET        "\ue148" // rotate-ccw
#define ICON_CAMERA       "\ue1a5" // video
#define ICON_VIDEO        "\ue1a5" // video
#define ICON_USER         "\ue19f" // user
#define ICON_SETTINGS     "\ue30b" // cog
#define ICON_REFRESH      "\ue145" // refresh-cw
#define ICON_HELP         "\ue082" // help-circle
#define ICON_INFO         "\ue0f9" // info
#define ICON_CLOSE        "\ue1b2" // x
#define ICON_MOVE         "\ue121" // move
#define ICON_FILE_PLUS    "\ue0c9" // file-plus
#define ICON_LIST         "\ue106" // list
#define ICON_GRID         "\ue0ff" // layout-grid
#define ICON_MORE         "\ue0b7" // ellipsis-vertical
#define ICON_SORT         "\ue37d" // arrow-up-down
#define ICON_FILTER       "\ue0dc" // filter
#define ICON_CALENDAR     "\ue063" // calendar
#define ICON_CLOCK        "\ue250" // clock-3
#define ICON_WORLD        "\ue0e8" // globe
#define ICON_WARNING      "\ue193" // triangle-alert

// Layers and viewport guides
#define ICON_TITLE      "\ue198" // type
#define ICON_SHAPE      "\ue4b3" // shapes
#define ICON_RECTANGLE  "\ue167" // square
#define ICON_ELLIPSE    "\ue076" // circle
#define ICON_TRIANGLE   "\ue192" // triangle
#define ICON_POLYGON    "\ue0f3" // hexagon
#define ICON_BLUR       "\ue0b5" // droplets
#define ICON_MOSAIC     "\ue0ff" // layout-grid
#define ICON_MOTION     "\ue1b0" // wind
#define ICON_RADIAL     "\ue29e" // focus
#define ICON_LIGHT_POINT "\ue1c2" // lightbulb
#define ICON_LIGHT_SPOT  "\ue682" // spotlight
#define ICON_LIGHT_STRIP "\ue27a" // align-vertical-space-around
#define ICON_LIGHT_AMBIENT "\ue299" // sun-dim
#define ICON_VANILLA     "\ue4fa" // blocks
#define ICON_VANILLA_ITEM "\ue5c6" // pickaxe
#define ICON_STANDIN     "\ue1a4" // users
#define ICON_HIRE        "\ue1a2" // user-plus
#define ICON_MARK_IN     "\ue238" // flag-triangle-right
#define ICON_MARK_OUT    "\ue237" // flag-triangle-left
#define ICON_MODEL      "\ue524" // cuboid
#define ICON_FILM       "\ue0d0" // film
#define ICON_THIRDS     "\ue0ef" // hash
#define ICON_STILL      "\ue064" // camera

// Preview page
#define ICON_HUD         "\ue11d" // monitor
#define ICON_CHAT        "\ue117" // message-square
#define ICON_BOSS_BAR    "\ue221" // skull
#define ICON_HEADING     "\ue384" // heading
#define ICON_SCOREBOARD  "\ue1d1" // list-ordered
#define ICON_ACTION_BAR  "\ue1b4" // zap
#define ICON_HOTBAR      "\ue4ce" // gallery-horizontal
#define ICON_VOLUME      "\ue1ab" // volume-2
#define ICON_PAUSE_SOUND "\ue12e" // pause
#define ICON_SUN         "\ue178" // sun
#define ICON_SNOWFLAKE   "\ue165" // snowflake
#define ICON_CLOUD_OFF   "\ue08d" // cloud-off
#define ICON_FOG         "\ue214" // cloud-fog
#define ICON_PALETTE     "\ue1dd" // palette
#define ICON_WEATHER     "\ue08e" // cloud-rain
#define ICON_FOV         "\ue536" // scan-eye
#define ICON_GRAPHICS    "\ue29a" // sliders-horizontal
#define ICON_PACKS       "\ue129" // package
#define ICON_EXPOSURE    "\ue2b1" // sun-medium
#define ICON_TEMPERATURE "\ue186" // thermometer
#define ICON_TINT        "\ue0b4" // droplet
#define ICON_CONTRAST    "\ue09d" // contrast
#define ICON_BOLD        "\ue05d" // bold
#define ICON_ITALIC      "\ue0fb" // italic
#define ICON_POSITION    "\ue121" // move
#define ICON_SCALE       "\ue2ec" // scaling
#define ICON_ROTATE      "\ue149" // rotate-cw
#define ICON_OPACITY     "\ue59c" // blend
#define ICON_FONT        "\ue3d9" // case-sensitive
#define ICON_CHEVRON_LEFT  "\ue06e" // chevron-left
#define ICON_CHEVRON_RIGHT "\ue06f" // chevron-right
#define ICON_GRAPH         "\ue60d" // chart-spline

// Track status
#define ICON_TRACK_ACTIVE "\ue345" // circle-dot
#define ICON_TRACK_OFF    "\ue076" // circle

// Transitions
#define ICON_TRANSITION "\ue417" // arrow-right-left

// Render / loading state
#define ICON_RENDER "\ue10a" // loader-circle (rotating)
#define ICON_LOADER "\ue109" // loader

// Menu layout
#define ICON_PANEL_LEFT "\ue12a" // panel-left
#define ICON_BACK       "\ue048" // arrow-left (←)

} // namespace Playback::ReplayEditor::Icons
