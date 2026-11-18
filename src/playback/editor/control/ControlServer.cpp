// First: httplib brings winsock2.h, which must come before anything that includes windows.h.
#include "httplib.h"

#include "ControlServer.h"

#include "playback/Playback.h"
#include "playback/editor/ui/ReplayEditor.h"
#include "playback/state/EditorAction.h"
#include "playback/state/EditorContext.h"
#include "playback/state/EditorState.h"

#include "magic_enum/magic_enum.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace playback::editor::control {

namespace {

using json = nlohmann::json;
using namespace playback::state;
namespace model = playback::state::editing::model;

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

json toJson(model::GlobalPack const& pack) {
    return {
        {"id",      pack.id     },
        {"version", pack.version},
        {"name",    pack.name   },
    };
}

json toJson(model::WorldSettings const& world) {
    auto packs = json::array();
    for (auto const& pack : world.globalPacks) packs.push_back(toJson(pack));
    return {
        {"overrideTime",        world.overrideTime                   },
        {"timeOfDay",           world.timeOfDay                      },
        {"freezeTime",          world.freezeTime                     },
        {"hideClouds",          world.hideClouds                     },
        {"overrideFog",         world.overrideFog                    },
        {"fogStart",            world.fogStart                       },
        {"fogEnd",              world.fogEnd                         },
        {"overrideFogColor",    world.overrideFogColor               },
        {"fogColor",            world.fogColor                       },
        {"overrideWeather",     world.overrideWeather                },
        {"weather",             magic_enum::enum_name(world.weather) },
        {"overrideFov",         world.overrideFov                    },
        {"fov",                 world.fov                            },
        {"overrideGraphics",    world.overrideGraphics               },
        {"graphics",            magic_enum::enum_name(world.graphics)},
        {"shaderOptions",       world.shaderOptions                  },
        {"overrideGlobalPacks", world.overrideGlobalPacks            },
        {"globalPacks",         std::move(packs)                     },
        {"overrideColor",       world.overrideColor                  },
        {"exposure",            world.exposure                       },
        {"temperature",         world.temperature                    },
        {"tint",                world.tint                           },
    };
}

json toJson(model::FollowSettings const& follow) {
    return {
        {"style",      magic_enum::enum_name(follow.style)                             },
        {"distance",   follow.distance                                                 },
        {"height",     follow.height                                                   },
        {"side",       follow.side                                                     },
        {"orbitSpeed", follow.orbitSpeed                                               },
        {"orbitStart", follow.orbitStart                                               },
        {"tripod",     json::array({follow.tripod.x, follow.tripod.y, follow.tripod.z})},
        {"hasTripod",  follow.hasTripod                                                },
        {"smoothing",  follow.smoothing                                                },
    };
}

json toJson(model::ShakeSettings const& shake) {
    return {
        {"positionAmount", shake.positionAmount},
        {"rotationAmount", shake.rotationAmount},
        {"rollAmount",     shake.rollAmount    },
        {"seed",           shake.seed          },
    };
}

json stateJson(EditorState const& state) {
    json result{
        {"replayVisible",  state.replayVisible            },
        {"editorVisible",  state.editorVisible            },
        {"paused",         state.paused                   },
        {"currentTick",    state.currentTick              },
        {"totalTicks",     state.totalTicks               },
        {"playbackSpeed",  state.playbackSpeed            },
        {"activeCameraId", state.activeCameraId           },
        {"following",      state.followingRecordedPlayer  },
        {"followThird",    state.followThirdPerson        },
        {"canUndo",        state.canUndo                  },
        {"canRedo",        state.canRedo                  },
        {"projectFile",    state.persistence.projectFile  },
        {"projectDirty",   state.persistence.dirty        },
        {"export",
         {{"state", magic_enum::enum_name(state.exportStatus.state)},
          {"message", state.exportStatus.message},
          {"writtenFrames", state.exportStatus.writtenFrames},
          {"totalFrames", state.exportStatus.totalFrames}}},
    };
    auto players = json::array();
    for (auto const& player : state.replayPlayers) {
        players.push_back({
            {"uuid",     player.uuid    },
            {"name",     player.name    },
            {"recorder", player.recorder},
            {"present",  player.present }
        });
    }
    result["players"] = std::move(players);

    auto installedPacks = json::array();
    if (state.installedGlobalPacks) {
        for (auto const& pack : *state.installedGlobalPacks) installedPacks.push_back(toJson(pack));
    }
    result["installedGlobalPacks"] = std::move(installedPacks);

    if (auto const& project = state.project) {
        auto cameras = json::array();
        for (auto const& camera : project->cameras) {
            auto keys = json::array();
            for (auto const& [tick, key] : camera.keysByTick) {
                keys.push_back({
                    {"tick",          tick                                                         },
                    {"interpolation", magic_enum::enum_name(key.interpolationType)                 },
                    {"position",      json::array({key.position.x, key.position.y, key.position.z})},
                    {"rotation",      json::array({key.yaw, key.pitch, key.roll})                  },
                    {"fov",           key.fov                                                      },
                    {"shake",         key.shake                                                    },
                    {"shakeFrequency", key.shakeFrequency                                          },
                    {"bezier",
                     json::array(
                         {json::array({key.bezierCtrl1.x, key.bezierCtrl1.y}),
                          json::array({key.bezierCtrl2.x, key.bezierCtrl2.y})}
                     )                                                                             },
                    {"subject",       key.subject                                                  }
                });
            }
            json entry{
                {"id",      camera.id                         },
                {"name",    camera.name                       },
                {"kind",    magic_enum::enum_name(camera.kind)},
                {"enabled", camera.enabled                    },
                {"locked",  camera.locked                     },
                {"keys",    std::move(keys)                   },
            };
            if (camera.kind == model::CameraTrackKind::ThirdPerson) entry["follow"] = toJson(camera.follow);
            if (camera.kind == model::CameraTrackKind::Keyframe) entry["shake"] = toJson(camera.shake);
            cameras.push_back(std::move(entry));
        }
        auto markers = json::array();
        for (auto const& marker : project->markers) {
            markers.push_back({
                {"id",    marker.id   },
                {"label", marker.label},
                {"tick",  marker.tick }
            });
        }
        auto poses = json::array();
        for (auto const& pose : project->poses) {
            poses.push_back({
                {"id",        pose.id       },
                {"subject",   pose.subject  },
                {"animation", pose.animation},
                {"startTick", pose.startTick},
                {"endTick",   pose.endTick  },
                {"hideItems", pose.hideItems},
            });
        }
        result["project"] = {
            {"cameras", std::move(cameras)    },
            {"markers", std::move(markers)    },
            {"poses",   std::move(poses)      },
            {"world",   toJson(project->world)},
        };
    }

    auto replays = json::array();
    if (auto const& snapshot = state.browser.snapshot) {
        for (auto const& entry : snapshot->replays) {
            replays.push_back({
                {"replayId",   entry.replayId                                              },
                {"name",       entry.displayName()                                         },
                {"world",      entry.worldName                                             },
                {"path",       reinterpret_cast<char const*>(entry.path.u8string().c_str())},
                {"totalTicks", entry.totalTicks                                            },
                {"canOpen",    entry.canOpen                                               },
                {"problem",    entry.problem                                               }
            });
        }
    }
    result["browser"] = {
        {"visible", state.browser.visible},
        {"busy",    state.browser.busy() },
        {"error",   state.browser.error  },
        {"replays", std::move(replays)   },
    };
    return result;
}

model::Vec3 vec3From(json const& node, model::Vec3 fallback) {
    if (node.is_array() && node.size() == 3) return {node[0].get<float>(), node[1].get<float>(), node[2].get<float>()};
    if (node.is_object())
        return {node.value("x", fallback.x), node.value("y", fallback.y), node.value("z", fallback.z)};
    return fallback;
}

// The project's world settings with the fields `node` names replaced.
model::WorldSettings worldFrom(json const& node, model::WorldSettings world) {
    world.overrideTime     = node.value("overrideTime", world.overrideTime);
    world.timeOfDay        = node.value("timeOfDay", world.timeOfDay);
    world.freezeTime       = node.value("freezeTime", world.freezeTime);
    world.hideClouds       = node.value("hideClouds", world.hideClouds);
    world.overrideFog      = node.value("overrideFog", world.overrideFog);
    world.fogStart         = node.value("fogStart", world.fogStart);
    world.fogEnd           = node.value("fogEnd", world.fogEnd);
    world.overrideFogColor = node.value("overrideFogColor", world.overrideFogColor);
    if (auto const color = node.find("fogColor"); color != node.end() && color->is_array() && color->size() == 3) {
        world.fogColor = {(*color)[0].get<float>(), (*color)[1].get<float>(), (*color)[2].get<float>()};
    }
    world.overrideWeather  = node.value("overrideWeather", world.overrideWeather);
    if (auto const weather = node.find("weather"); weather != node.end() && weather->is_string()) {
        if (auto const parsed = magic_enum::enum_cast<model::WeatherStyle>(weather->get<std::string>())) {
            world.weather = *parsed;
        }
    }
    world.overrideFov      = node.value("overrideFov", world.overrideFov);
    world.fov              = node.value("fov", world.fov);
    world.overrideGraphics = node.value("overrideGraphics", world.overrideGraphics);
    if (auto const graphics = node.find("graphics"); graphics != node.end() && graphics->is_string()) {
        if (auto const parsed = magic_enum::enum_cast<model::GraphicsStyle>(graphics->get<std::string>())) {
            world.graphics = *parsed;
        }
    }
    // Merged per pack: {"<pack>": {"NAME": "value"}}; a null value drops Playback's value for that option.
    if (auto const packs = node.find("shaderOptions"); packs != node.end() && packs->is_object()) {
        for (auto const& [pack, options] : packs->items()) {
            if (!options.is_object()) continue;
            auto& values = world.shaderOptions[pack];
            for (auto const& [name, value] : options.items()) {
                if (value.is_null()) values.erase(name);
                else values.insert_or_assign(name, value.is_string() ? value.get<std::string>() : value.dump());
            }
            if (values.empty()) world.shaderOptions.erase(pack);
        }
    }
    world.overrideGlobalPacks = node.value("overrideGlobalPacks", world.overrideGlobalPacks);
    // The whole list, top priority first: [{"id", "version", "name"}].
    if (auto const packs = node.find("globalPacks"); packs != node.end() && packs->is_array()) {
        world.globalPacks.clear();
        for (auto const& pack : *packs) {
            world.globalPacks.push_back({
                pack.value("id", std::string{}),
                pack.value("version", std::string{}),
                pack.value("name", std::string{}),
            });
        }
    }
    world.overrideColor = node.value("overrideColor", world.overrideColor);
    world.exposure      = node.value("exposure", world.exposure);
    world.temperature   = node.value("temperature", world.temperature);
    world.tint          = node.value("tint", world.tint);
    return world.sanitized();
}

// A track's follow settings with the fields `node` names replaced.
model::FollowSettings followFrom(json const& node, model::FollowSettings follow) {
    if (auto const style = node.find("style"); style != node.end() && style->is_string()) {
        if (auto const parsed = magic_enum::enum_cast<model::FollowStyle>(style->get<std::string>()))
            follow.style = *parsed;
    }
    follow.distance   = node.value("distance", follow.distance);
    follow.height     = node.value("height", follow.height);
    follow.side       = node.value("side", follow.side);
    follow.orbitSpeed = node.value("orbitSpeed", follow.orbitSpeed);
    follow.orbitStart = node.value("orbitStart", follow.orbitStart);
    follow.smoothing  = node.value("smoothing", follow.smoothing);
    if (auto const tripod = node.find("tripod"); tripod != node.end()) {
        follow.tripod    = vec3From(*tripod, follow.tripod);
        follow.hasTripod = true;
    }
    follow.hasTripod = node.value("hasTripod", follow.hasTripod);
    return follow.sanitized();
}

// A track's shake settings with the fields `node` names replaced.
model::ShakeSettings shakeFrom(json const& node, model::ShakeSettings shake) {
    shake.positionAmount = node.value("positionAmount", shake.positionAmount);
    shake.rotationAmount = node.value("rotationAmount", shake.rotationAmount);
    shake.rollAmount     = node.value("rollAmount", shake.rollAmount);
    shake.seed           = node.value("seed", shake.seed);
    return shake.sanitized();
}

// Export settings over the defaults; fields it leaves out keep them. The tick range and resolution have no usable
// default, so a caller sets them.
exporting::ExportSettings exportFrom(json const& node) {
    exporting::ExportSettings settings;
    if (auto const directory = node.find("outputDirectory"); directory != node.end() && directory->is_string()) {
        auto const text          = directory->get<std::string>();
        settings.outputDirectory = std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    settings.outputName          = node.value("outputName", settings.outputName);
    settings.startTick           = node.value("startTick", settings.startTick);
    settings.endTick             = node.value("endTick", settings.endTick);
    settings.frameRate.numerator = node.value("fps", settings.frameRate.numerator);
    if (auto const format = node.find("format"); format != node.end() && format->is_string()) {
        if (auto const parsed = magic_enum::enum_cast<exporting::ExportFormat>(format->get<std::string>())) {
            settings.format = *parsed;
        }
    }
    if (auto const resolution = node.find("resolution");
        resolution != node.end() && resolution->is_array() && resolution->size() == 2) {
        settings.resolutionX = (*resolution)[0].get<uint32_t>();
        settings.resolutionY = (*resolution)[1].get<uint32_t>();
    }
    settings.ssaa              = node.value("ssaa", settings.ssaa);
    settings.warmupFrames      = node.value("warmupFrames", settings.warmupFrames);
    settings.extraRenders      = node.value("extraRenders", settings.extraRenders);
    settings.motionBlurSamples = node.value("motionBlurSamples", settings.motionBlurSamples);
    settings.motionBlurShutter = node.value("motionBlurShutter", settings.motionBlurShutter);
    settings.audio             = node.value("audio", settings.audio);
    settings.hideHud           = node.value("hideHud", settings.hideHud);
    return settings;
}

// The EditorAction `body` describes; fields it leaves out keep the action's defaults.
std::optional<EditorAction> actionFrom(json const& body, EditorState const& state, std::string& error) {
    if (!body.is_object()) {
        error = "the body must be a JSON object";
        return std::nullopt;
    }
    auto const type = magic_enum::enum_cast<EditorActionType>(body.value("type", std::string{}));
    if (!type) {
        error = "unknown action type; GET /actions lists them";
        return std::nullopt;
    }
    EditorAction action{*type};
    action.tick          = body.value("tick", action.tick);
    action.secondaryTick = body.value("secondaryTick", action.secondaryTick);
    action.kind          = body.value("kind", action.kind);
    action.speed         = body.value("speed", action.speed);
    action.value         = body.value("value", action.value);
    action.replayId      = body.value("replayId", action.replayId);
    action.name          = body.value("name", action.name);
    action.id            = body.value("id", action.id);
    action.secondaryId   = body.value("secondaryId", action.secondaryId);
    if (auto const path = body.find("path"); path != body.end() && path->is_string()) {
        auto const text = path->get<std::string>();
        action.path     = std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    if (auto const position = body.find("position"); position != body.end()) {
        action.position = vec3From(*position, action.position);
    }
    if (auto const ids = body.find("replayIds"); ids != body.end() && ids->is_array()) {
        for (auto const& id : *ids) action.replayIds.push_back(id.get<std::string>());
    }
    if (auto const refs = body.find("keyframeRefs"); refs != body.end() && refs->is_array()) {
        for (auto const& ref : *refs) {
            action.keyframeRefs.push_back({ref.value("cameraId", std::string{}), ref.value("tick", 0), std::nullopt});
        }
    }
    if (auto const world = body.find("worldSettings"); world != body.end()) {
        action.worldSettings = worldFrom(*world, state.project ? state.project->world : model::WorldSettings{});
    }
    if (auto const poses = body.find("poseClips"); poses != body.end() && poses->is_array()) {
        auto& clips = action.poseClips.emplace();
        for (auto const& entry : *poses) {
            model::PoseClip clip;
            clip.id        = entry.value("id", std::string{});
            clip.subject   = entry.value("subject", std::string{});
            clip.animation = entry.value("animation", std::string{model::PoseClip::DefaultAnimation});
            clip.startTick = entry.value("startTick", 0);
            clip.endTick   = entry.value("endTick", 0);
            clip.hideItems = entry.value("hideItems", true);
            clips.push_back(std::move(clip));
        }
    }
    if (auto const follow = body.find("followSettings"); follow != body.end()) {
        model::FollowSettings current;
        if (state.project) {
            auto const camera = std::ranges::find(state.project->cameras, action.id, &model::CameraEntity::id);
            if (camera != state.project->cameras.end()) current = camera->follow;
        }
        action.followSettings = followFrom(*follow, current);
    }
    if (auto const shake = body.find("shakeSettings"); shake != body.end()) {
        model::ShakeSettings current;
        if (state.project) {
            auto const camera = std::ranges::find(state.project->cameras, action.id, &model::CameraEntity::id);
            if (camera != state.project->cameras.end()) current = camera->shake;
        }
        action.shakeSettings = shakeFrom(*shake, current);
    }
    if (auto const frequency = body.find("shakeFrequency"); frequency != body.end()) {
        action.shakeFrequency = frequency->get<float>();
    }
    if (auto const bezier = body.find("bezierControls");
        bezier != body.end() && bezier->is_array() && bezier->size() == 2) {
        action.bezierControls = std::array{
            model::Vec2{(*bezier)[0][0].get<float>(), (*bezier)[0][1].get<float>()},
            model::Vec2{(*bezier)[1][0].get<float>(), (*bezier)[1][1].get<float>()},
        };
    }
    if (auto const settings = body.find("exportSettings"); settings != body.end() && settings->is_object()) {
        action.exportSettings = exportFrom(*settings);
    }
    return action;
}

struct Server {
    httplib::Server http;
    std::thread     thread;
};

std::mutex              gServerMutex;
std::unique_ptr<Server> gServer;

} // namespace

void startControlServer(EditorContext& context, int port) {
    std::scoped_lock lock(gServerMutex);
    if (gServer) return;
    auto server = std::make_unique<Server>();
    server->http.Get("/state", [&context](httplib::Request const&, httplib::Response& response) {
        response.set_content(stateJson(context.snapshot()).dump(), "application/json");
    });
    server->http.Get("/actions", [](httplib::Request const&, httplib::Response& response) {
        auto names = json::array();
        for (auto const name : magic_enum::enum_names<EditorActionType>()) names.push_back(name);
        response.set_content(names.dump(), "application/json");
    });
    // Selects a keyframe as clicking it would, so the inspector shows it: {"cameraId": "...", "tick": 120}.
    server->http.Post("/select", [](httplib::Request const& request, httplib::Response& response) {
        auto const body = json::parse(request.body, nullptr, false);
        if (!body.is_object() || !body.contains("cameraId") || !body["cameraId"].is_string() || !body.contains("tick")
            || !body["tick"].is_number_integer()) {
            response.status = 400;
            response.set_content(
                R"({"error":"expected {\"cameraId\": string, \"tick\": integer}"})",
                "application/json"
            );
            return;
        }
        ui::ReplayEditor::getInstance().requestKeyframeSelection(
            body["cameraId"].get<std::string>(),
            body["tick"].get<int>()
        );
        response.set_content(R"({"ok":true})", "application/json");
    });
    server->http.Post("/action", [&context](httplib::Request const& request, httplib::Response& response) {
        auto const                  body = json::parse(request.body, nullptr, false);
        std::string                 error;
        std::optional<EditorAction> action;
        try {
            action = actionFrom(body, context.snapshot(), error);
        } catch (json::exception const& exception) {
            error = exception.what();
        }
        if (!action) {
            response.status = 400;
            response.set_content(
                json{
                    {"error", error}
            }.dump(),
                "application/json"
            );
            return;
        }
        context.submit(std::move(*action));
        response.set_content(R"({"ok":true})", "application/json");
    });
    // Loopback only: the API drives the game, so nothing outside this machine may reach it.
    if (!server->http.bind_to_port("127.0.0.1", port)) {
        logger().error("Unable to start the control API on 127.0.0.1:{}", port);
        return;
    }
    server->thread = std::thread([http = &server->http] { http->listen_after_bind(); });
    gServer        = std::move(server);
    logger().info("Control API listening on http://127.0.0.1:{}", port);
}

void stopControlServer() {
    std::scoped_lock lock(gServerMutex);
    if (!gServer) return;
    gServer->http.stop();
    if (gServer->thread.joinable()) gServer->thread.join();
    gServer.reset();
}

} // namespace playback::editor::control
