#include "playback/state/editing/ProjectSerializer.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

namespace playback::state::editing {

namespace {

using json = nlohmann::ordered_json;
using namespace model;

float finiteOr(json const& node, char const* key, float fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_number()) return fallback;
    auto const result = value->get<float>();
    return std::isfinite(result) ? result : fallback;
}

int intOr(json const& node, char const* key, int fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_number_integer()) return fallback;
    return value->get<int>();
}

bool boolOr(json const& node, char const* key, bool fallback) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_boolean()) return fallback;
    return value->get<bool>();
}

std::string stringOr(json const& node, char const* key) {
    auto const value = node.find(key);
    if (value == node.end() || !value->is_string()) return {};
    return value->get<std::string>();
}

json const* objectAt(json const& node, char const* key) {
    auto const value = node.find(key);
    return value != node.end() && value->is_object() ? &*value : nullptr;
}

json const* arrayAt(json const& node, char const* key) {
    auto const value = node.find(key);
    return value != node.end() && value->is_array() ? &*value : nullptr;
}

template <class Enum>
Enum enumOr(json const& node, char const* key, Enum fallback, int maxValue) {
    int const raw = intOr(node, key, static_cast<int>(fallback));
    if (raw < 0 || raw > maxValue) return fallback;
    return static_cast<Enum>(raw);
}

json toJson(Vec2 const& value) {
    return {
        {"x", value.x},
        {"y", value.y}
    };
}

json toJson(Vec3 const& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z}
    };
}

json toJson(Color4 const& value) {
    return {
        {"r", value.r},
        {"g", value.g},
        {"b", value.b},
        {"a", value.a}
    };
}

Vec2 vec2From(json const& node, char const* key, Vec2 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {finiteOr(*object, "x", fallback.x), finiteOr(*object, "y", fallback.y)};
}

Vec3 vec3From(json const& node, char const* key, Vec3 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {finiteOr(*object, "x", fallback.x), finiteOr(*object, "y", fallback.y), finiteOr(*object, "z", fallback.z)};
}

Color4 colorFrom(json const& node, char const* key, Color4 fallback) {
    auto const* object = objectAt(node, key);
    if (!object) return fallback;
    return {
        finiteOr(*object, "r", fallback.r),
        finiteOr(*object, "g", fallback.g),
        finiteOr(*object, "b", fallback.b),
        finiteOr(*object, "a", fallback.a)
    };
}

json toJson(CameraKeyframe const& key) {
    return {
        {"position",          toJson(key.position)                   },
        {"yaw",               key.yaw                                },
        {"pitch",             key.pitch                              },
        {"roll",              key.roll                               },
        {"interpolationType", static_cast<int>(key.interpolationType)},
        {"bezierCtrl1",       toJson(key.bezierCtrl1)                },
        {"bezierCtrl2",       toJson(key.bezierCtrl2)                },
        {"subject",           key.subject                            }
    };
}

CameraKeyframe keyframeFrom(json const& node) {
    CameraKeyframe key;
    key.position          = vec3From(node, "position", key.position);
    key.yaw               = finiteOr(node, "yaw", key.yaw);
    key.pitch             = finiteOr(node, "pitch", key.pitch);
    key.roll              = finiteOr(node, "roll", key.roll);
    key.interpolationType = enumOr(
        node,
        "interpolationType",
        key.interpolationType,
        static_cast<int>(CameraInterpolationType::CubicBezier)
    );
    key.bezierCtrl1 = vec2From(node, "bezierCtrl1", key.bezierCtrl1);
    key.bezierCtrl2 = vec2From(node, "bezierCtrl2", key.bezierCtrl2);
    key.subject     = stringOr(node, "subject");
    return key;
}

json toJson(ValueChannels const& channels) {
    json result = json::object();
    for (auto const& [name, channel] : channels) {
        if (channel.empty()) continue;
        auto keys = json::array();
        for (auto const& [tick, key] : channel) {
            keys.push_back({
                {"tick",   tick                         },
                {"value",  key.value                    },
                {"easing", static_cast<int>(key.easing)},
            });
        }
        result[name] = std::move(keys);
    }
    return result;
}

ValueChannels channelsFrom(json const& node, char const* key) {
    ValueChannels channels;
    auto const*   object = objectAt(node, key);
    if (!object) return channels;
    for (auto const& [name, keys] : object->items()) {
        if (!keys.is_array()) continue;
        auto& channel = channels[name];
        for (auto const& entry : keys) {
            if (!entry.is_object()) continue;
            channel.insert_or_assign(
                intOr(entry, "tick", 0),
                ValueKey{finiteOr(entry, "value", 0.0f), enumOr(entry, "easing", KeyEasing::EaseInOut, static_cast<int>(KeyEasing::Hold))}
            );
        }
    }
    pruneChannels(channels);
    return channels;
}

// Projects from before properties keyed on their own held FOV and shake in every Transform key: a value the keys
// all share becomes the static value, one that changes becomes the property's own keys.
void migrateKeyProperty(CameraEntity& camera, json const& keys, char const* field, char const* property, float fallback) {
    ValueChannel channel;
    for (auto const& entry : keys) {
        if (!entry.is_object() || !entry.contains(field)) continue;
        float value = finiteOr(entry, field, fallback);
        if (std::string_view{property} == CameraProperty::ShakeStrength) value = sanitizedShakeStrength(value);
        if (std::string_view{property} == CameraProperty::ShakeFrequency) value = sanitizedShakeFrequency(value);
        channel.insert_or_assign(intOr(entry, "tick", 0), ValueKey{value, KeyEasing::Linear});
    }
    if (channel.empty()) return;
    float const first = channel.begin()->second.value;
    if (std::ranges::all_of(channel, [first](auto const& entry) { return entry.second.value == first; })) {
        setCameraStaticValue(camera, property, first);
    } else {
        camera.channels[property] = std::move(channel);
    }
}

json toJson(CameraEntity const& camera) {
    auto keys = json::array();
    for (auto const& [tick, key] : camera.keysByTick) {
        auto entry    = toJson(key);
        entry["tick"] = tick;
        keys.push_back(std::move(entry));
    }

    json result{
        {"id",                camera.id                    },
        {"name",              camera.name                  },
        {"kind",              static_cast<int>(camera.kind)},
        {"enabled",           camera.enabled               },
        {"locked",            camera.locked                },
        {"bindingEntityUuid", camera.bindingEntityUuid     },
        {"bindingMode",       camera.bindingMode           },
        {"bindingDamping",    camera.bindingDamping        },
        {"keys",              std::move(keys)              }
    };
    if (camera.kind == CameraTrackKind::Keyframe) {
        result["shake"] = {
            {"positionAmount", camera.shake.positionAmount},
            {"rotationAmount", camera.shake.rotationAmount},
            {"rollAmount",     camera.shake.rollAmount    },
            {"seed",           camera.shake.seed          }
        };
        result["fov"]            = camera.fov;
        result["shakeStrength"]  = camera.shakeStrength;
        result["shakeFrequency"] = camera.shakeFrequency;
        result["channels"]       = toJson(camera.channels);
    }
    if (camera.limiter) {
        result["limiter"] = {
            {"min",     toJson(camera.limiter->min)},
            {"max",     toJson(camera.limiter->max)},
            {"enabled", camera.limiter->enabled    }
        };
    }
    if (camera.kind == CameraTrackKind::ThirdPerson) {
        result["follow"] = {
            {"style",      static_cast<int>(camera.follow.style)},
            {"distance",   camera.follow.distance               },
            {"height",     camera.follow.height                 },
            {"side",       camera.follow.side                   },
            {"orbitSpeed", camera.follow.orbitSpeed             },
            {"orbitStart", camera.follow.orbitStart             },
            {"tripod",     toJson(camera.follow.tripod)         },
            {"hasTripod",  camera.follow.hasTripod              },
            {"smoothing",  camera.follow.smoothing              }
        };
    }
    return result;
}

CameraEntity cameraFrom(json const& node) {
    CameraEntity camera;
    camera.id                = stringOr(node, "id");
    camera.name              = stringOr(node, "name");
    camera.kind = enumOr(node, "kind", camera.kind, static_cast<int>(CameraTrackKind::ThirdPerson));
    camera.enabled           = boolOr(node, "enabled", true);
    camera.locked            = boolOr(node, "locked", false);
    camera.bindingEntityUuid = stringOr(node, "bindingEntityUuid");
    camera.bindingMode       = intOr(node, "bindingMode", 0);
    camera.bindingDamping    = finiteOr(node, "bindingDamping", 0.1f);

    if (auto const* keys = arrayAt(node, "keys")) {
        for (auto const& entry : *keys) {
            if (!entry.is_object()) continue;
            camera.keysByTick.insert_or_assign(intOr(entry, "tick", 0), keyframeFrom(entry));
        }
        if (!node.contains("channels")) {
            migrateKeyProperty(camera, *keys, "fov", CameraProperty::Fov, camera.fov);
            migrateKeyProperty(camera, *keys, "shake", CameraProperty::ShakeStrength, 0.0f);
            migrateKeyProperty(camera, *keys, "shakeFrequency", CameraProperty::ShakeFrequency, camera.shakeFrequency);
        }
    }
    camera.fov            = finiteOr(node, "fov", camera.fov);
    camera.shakeStrength  = sanitizedShakeStrength(finiteOr(node, "shakeStrength", camera.shakeStrength));
    camera.shakeFrequency = sanitizedShakeFrequency(finiteOr(node, "shakeFrequency", camera.shakeFrequency));
    if (node.contains("channels")) camera.channels = channelsFrom(node, "channels");
    if (auto const* shake = objectAt(node, "shake")) {
        ShakeSettings const defaults;
        camera.shake.positionAmount = finiteOr(*shake, "positionAmount", defaults.positionAmount);
        camera.shake.rotationAmount = finiteOr(*shake, "rotationAmount", defaults.rotationAmount);
        camera.shake.rollAmount     = finiteOr(*shake, "rollAmount", defaults.rollAmount);
        if (auto const seed = shake->find("seed"); seed != shake->end() && seed->is_number_unsigned()) {
            camera.shake.seed = seed->get<uint32_t>();
        }
        camera.shake = camera.shake.sanitized();
    }
    if (auto const* follow = objectAt(node, "follow")) {
        FollowSettings const defaults;
        camera.follow.style    = enumOr(*follow, "style", defaults.style, static_cast<int>(model::FollowStyle::Tripod));
        camera.follow.distance = finiteOr(*follow, "distance", defaults.distance);
        camera.follow.height   = finiteOr(*follow, "height", defaults.height);
        camera.follow.side     = finiteOr(*follow, "side", defaults.side);
        camera.follow.orbitSpeed = finiteOr(*follow, "orbitSpeed", defaults.orbitSpeed);
        camera.follow.orbitStart = finiteOr(*follow, "orbitStart", defaults.orbitStart);
        camera.follow.tripod     = vec3From(*follow, "tripod", defaults.tripod);
        camera.follow.hasTripod  = boolOr(*follow, "hasTripod", defaults.hasTripod);
        camera.follow.smoothing  = finiteOr(*follow, "smoothing", defaults.smoothing);
        camera.follow            = camera.follow.sanitized();
    }
    if (auto const* limiter = objectAt(node, "limiter")) {
        camera.limiter = CameraLimiter{
            vec3From(*limiter, "min", {}),
            vec3From(*limiter, "max", {}),
            boolOr(*limiter, "enabled", false)
        };
    }
    return camera;
}

json toJson(WorldActorSegment const& segment) {
    return {
        {"id",         segment.id           },
        {"startTick",  segment.startTick    },
        {"endTick",    segment.endTick      },
        {"sourceTick", segment.sourceTick   },
        {"speed",      segment.speed        },
        {"color",      toJson(segment.color)},
        {"locked",     segment.locked       }
    };
}

WorldActorSegment worldActorSegmentFrom(json const& node) {
    WorldActorSegment segment;
    segment.id         = stringOr(node, "id");
    segment.startTick  = intOr(node, "startTick", 0);
    segment.endTick    = intOr(node, "endTick", 0);
    segment.sourceTick = intOr(node, "sourceTick", 0);
    segment.speed      = finiteOr(node, "speed", 1.0f);
    segment.color      = colorFrom(node, "color", segment.color);
    segment.locked     = boolOr(node, "locked", false);
    return segment;
}

json toJson(SubActor const& actor) {
    auto details = json::object();
    for (auto const& [key, value] : actor.agentDetails) details[key] = value;

    return {
        {"id",             actor.id                        },
        {"name",           actor.name                      },
        {"category",       static_cast<int>(actor.category)},
        {"position",       toJson(actor.position)          },
        {"rotation",       toJson(actor.rotation)          },
        {"agentDetails",   std::move(details)              },
        {"boundCameraIds", actor.boundCameraIds            }
    };
}

SubActor subActorFrom(json const& node) {
    SubActor actor;
    actor.id       = stringOr(node, "id");
    actor.name     = stringOr(node, "name");
    actor.category = enumOr(node, "category", actor.category, static_cast<int>(SubActorCategory::Entities));
    actor.position = vec3From(node, "position", {});
    actor.rotation = vec2From(node, "rotation", {});
    if (auto const* details = objectAt(node, "agentDetails")) {
        for (auto const& [key, value] : details->items()) {
            if (value.is_string()) actor.agentDetails.insert_or_assign(key, value.get<std::string>());
        }
    }
    if (auto const* bound = arrayAt(node, "boundCameraIds")) {
        for (auto const& entry : *bound) {
            if (entry.is_string()) actor.boundCameraIds.push_back(entry.get<std::string>());
        }
    }
    return actor;
}

json toJson(Marker const& marker) {
    return {
        {"id",    marker.id   },
        {"label", marker.label},
        {"tick",  marker.tick }
    };
}

json toJson(Clip const& clip) {
    return {
        {"id",                   clip.id                  },
        {"replayFile",           clip.replayFile          },
        {"inTick",               clip.inTick              },
        {"outTick",              clip.outTick             },
        {"trackTick",            clip.trackTick           },
        {"activeCameraTrackIdx", clip.activeCameraTrackIdx},
        {"speed",                clip.speed               },
        {"name",                 clip.name                },
        {"color",                toJson(clip.color)       },
        {"muted",                clip.muted               },
        {"locked",               clip.locked              }
    };
}

Clip clipFrom(json const& node) {
    Clip clip;
    clip.id                   = stringOr(node, "id");
    clip.replayFile           = stringOr(node, "replayFile");
    clip.inTick               = intOr(node, "inTick", 0);
    clip.outTick              = intOr(node, "outTick", 0);
    clip.trackTick            = intOr(node, "trackTick", 0);
    clip.activeCameraTrackIdx = intOr(node, "activeCameraTrackIdx", 0);
    clip.speed                = finiteOr(node, "speed", 1.0f);
    clip.name                 = stringOr(node, "name");
    clip.color                = colorFrom(node, "color", clip.color);
    clip.muted                = boolOr(node, "muted", false);
    clip.locked               = boolOr(node, "locked", false);
    return clip;
}

json toJson(Track const& track) {
    auto clips = json::array();
    for (auto const& clip : track.clips) clips.push_back(toJson(clip));

    return {
        {"id",      track.id                    },
        {"name",    track.name                  },
        {"kind",    static_cast<int>(track.kind)},
        {"visible", track.visible               },
        {"locked",  track.locked                },
        {"height",  track.height                },
        {"clips",   std::move(clips)            }
    };
}

Track trackFrom(json const& node) {
    Track track;
    track.id      = stringOr(node, "id");
    track.name    = stringOr(node, "name");
    track.kind    = enumOr(node, "kind", track.kind, static_cast<int>(TrackKind::Marker));
    track.visible = boolOr(node, "visible", true);
    track.locked  = boolOr(node, "locked", false);
    track.height  = intOr(node, "height", 48);
    if (auto const* clips = arrayAt(node, "clips")) {
        for (auto const& entry : *clips) {
            if (entry.is_object()) track.clips.push_back(clipFrom(entry));
        }
    }
    return track;
}

json toJson(Transition const& transition) {
    return {
        {"id",            transition.id                    },
        {"kind",          static_cast<int>(transition.kind)},
        {"durationTicks", transition.durationTicks         },
        {"easing",        transition.easing                },
        {"fromClipId",    transition.fromClipId            },
        {"toClipId",      transition.toClipId              },
        {"fadeColor",     toJson(transition.fadeColor)     }
    };
}

Transition transitionFrom(json const& node) {
    Transition transition;
    transition.id            = stringOr(node, "id");
    transition.kind          = enumOr(node, "kind", transition.kind, static_cast<int>(TransitionKind::CrossDissolve));
    transition.durationTicks = intOr(node, "durationTicks", 20);
    transition.easing        = intOr(node, "easing", 0);
    transition.fromClipId    = stringOr(node, "fromClipId");
    transition.toClipId      = stringOr(node, "toClipId");
    transition.fadeColor     = colorFrom(node, "fadeColor", transition.fadeColor);
    return transition;
}

json toJson(OverlayItem const& item) {
    auto const& t = item.transform;
    auto const& title = item.title;
    auto const& shape = item.shape;
    auto const& model = item.model;
    return {
        {"id",           item.id                       },
        {"name",         item.name                     },
        {"kind",         static_cast<int>(item.kind)   },
        {"startTick",    item.startTick                },
        {"endTick",      item.endTick                  },
        {"fadeInTicks",  item.fadeInTicks              },
        {"fadeOutTicks", item.fadeOutTicks             },
        {"visible",      item.visible                  },
        {"locked",       item.locked                   },
        {"transform",
         json{
             {"x", t.x},
             {"y", t.y},
             {"width", t.width},
             {"height", t.height},
             {"rotation", t.rotation},
             {"skewX", t.skewX},
             {"skewY", t.skewY},
             {"tiltX", t.tiltX},
             {"tiltY", t.tiltY},
             {"opacity", t.opacity},
         }                                             },
        {"title",
         json{
             {"text", title.text},
             {"fontFamily", title.fontFamily},
             {"fontSize", title.fontSize},
             {"bold", title.bold},
             {"italic", title.italic},
             {"align", static_cast<int>(title.align)},
             {"color", toJson(title.color)},
             {"outlineWidth", title.outlineWidth},
             {"outlineColor", toJson(title.outlineColor)},
         }                                             },
        {"shape",
         json{
             {"kind", static_cast<int>(shape.kind)},
             {"points", shape.points},
             {"fill", toJson(shape.fill)},
             {"strokeWidth", shape.strokeWidth},
             {"stroke", toJson(shape.stroke)},
             {"cornerRadius", shape.cornerRadius},
         }                                             },
        {"model",
         json{
             {"file", model.file},
             {"position", toJson(model.position)},
             {"yaw", model.yaw},
             {"pitch", model.pitch},
             {"roll", model.roll},
             {"scale", model.scale},
         }                                             },
        {"blur",
         json{
             {"style", static_cast<int>(item.blur.style)},
             {"amount", item.blur.amount},
             {"angle", item.blur.angle},
             {"area", static_cast<int>(item.blur.area)},
             {"feather", item.blur.feather},
             {"invert", item.blur.invert},
         }                                             },
        {"channels",     toJson(item.channels)         },
    };
}

OverlayItem overlayFrom(json const& node) {
    OverlayItem item;
    item.id           = stringOr(node, "id");
    item.name         = stringOr(node, "name");
    item.kind         = enumOr(node, "kind", item.kind, static_cast<int>(OverlayKind::Blur));
    item.startTick    = std::max(0, intOr(node, "startTick", item.startTick));
    item.endTick      = std::max(item.startTick + 1, intOr(node, "endTick", item.endTick));
    item.fadeInTicks  = std::max(0, intOr(node, "fadeInTicks", 0));
    item.fadeOutTicks = std::max(0, intOr(node, "fadeOutTicks", 0));
    item.visible      = boolOr(node, "visible", true);
    item.locked       = boolOr(node, "locked", false);
    if (auto const* t = objectAt(node, "transform")) {
        auto& out    = item.transform;
        out.x        = finiteOr(*t, "x", out.x);
        out.y        = finiteOr(*t, "y", out.y);
        out.width    = finiteOr(*t, "width", out.width);
        out.height   = finiteOr(*t, "height", out.height);
        out.rotation = finiteOr(*t, "rotation", out.rotation);
        out.skewX    = finiteOr(*t, "skewX", out.skewX);
        out.skewY    = finiteOr(*t, "skewY", out.skewY);
        out.tiltX    = finiteOr(*t, "tiltX", out.tiltX);
        out.tiltY    = finiteOr(*t, "tiltY", out.tiltY);
        out.opacity  = std::clamp(finiteOr(*t, "opacity", out.opacity), 0.0f, 1.0f);
    }
    if (auto const* title = objectAt(node, "title")) {
        auto& out        = item.title;
        out.text         = stringOr(*title, "text");
        out.fontFamily   = stringOr(*title, "fontFamily");
        if (out.fontFamily.empty()) out.fontFamily = TitleStyle{}.fontFamily;
        out.fontSize     = std::max(0.001f, finiteOr(*title, "fontSize", out.fontSize));
        out.bold         = boolOr(*title, "bold", out.bold);
        out.italic       = boolOr(*title, "italic", out.italic);
        out.align        = enumOr(*title, "align", out.align, static_cast<int>(TextAlign::Right));
        out.color        = colorFrom(*title, "color", out.color);
        out.outlineWidth = std::max(0.0f, finiteOr(*title, "outlineWidth", out.outlineWidth));
        out.outlineColor = colorFrom(*title, "outlineColor", out.outlineColor);
    }
    if (auto const* shape = objectAt(node, "shape")) {
        auto& out        = item.shape;
        out.kind         = enumOr(*shape, "kind", out.kind, static_cast<int>(ShapeKind::Polygon));
        out.points       = std::clamp(intOr(*shape, "points", out.points), 3, 64);
        out.fill         = colorFrom(*shape, "fill", out.fill);
        out.strokeWidth  = std::max(0.0f, finiteOr(*shape, "strokeWidth", out.strokeWidth));
        out.stroke       = colorFrom(*shape, "stroke", out.stroke);
        out.cornerRadius = std::max(0.0f, finiteOr(*shape, "cornerRadius", out.cornerRadius));
    }
    if (auto const* model = objectAt(node, "model")) {
        auto& out = item.model;
        out.file  = stringOr(*model, "file");
        if (auto const* position = objectAt(*model, "position")) {
            out.position = {
                finiteOr(*position, "x", 0.0f),
                finiteOr(*position, "y", 0.0f),
                finiteOr(*position, "z", 0.0f),
            };
        }
        out.yaw   = finiteOr(*model, "yaw", out.yaw);
        out.pitch = finiteOr(*model, "pitch", out.pitch);
        out.roll  = finiteOr(*model, "roll", out.roll);
        out.scale = std::max(0.001f, finiteOr(*model, "scale", out.scale));
    }
    if (auto const* blur = objectAt(node, "blur")) {
        auto& out   = item.blur;
        out.style   = enumOr(*blur, "style", out.style, static_cast<int>(BlurStyle::Radial));
        out.amount  = std::max(0.0f, finiteOr(*blur, "amount", out.amount));
        out.angle   = finiteOr(*blur, "angle", out.angle);
        out.area    = enumOr(*blur, "area", out.area, static_cast<int>(BlurArea::Ellipse));
        out.feather = std::max(0.0f, finiteOr(*blur, "feather", out.feather));
        out.invert  = boolOr(*blur, "invert", out.invert);
    }
    item.channels = channelsFrom(node, "channels");
    return item;
}

} // namespace

std::string serializeOverlays(std::vector<model::OverlayItem> const& overlays) {
    auto array = json::array();
    for (auto const& item : overlays) array.push_back(toJson(item));
    return array.dump();
}

bool deserializeOverlays(std::string const& payload, std::vector<model::OverlayItem>& overlays) {
    auto const document = json::parse(payload, nullptr, false);
    if (document.is_discarded() || !document.is_array()) return false;
    std::vector<model::OverlayItem> loaded;
    for (auto const& entry : document) {
        if (entry.is_object()) loaded.push_back(overlayFrom(entry));
    }
    overlays = std::move(loaded);
    return true;
}

std::string serializeProject(model::EditorStateExt const& project) {
    auto cameras = json::array();
    for (auto const& camera : project.cameras) cameras.push_back(toJson(camera));
    auto overlays = json::array();
    for (auto const& item : project.overlays) overlays.push_back(toJson(item));

    auto worldSegments = json::array();
    for (auto const& segment : project.worldActor.segments) worldSegments.push_back(toJson(segment));

    auto subActors = json::array();
    for (auto const& actor : project.worldActor.subActors) subActors.push_back(toJson(actor));

    auto markers = json::array();
    for (auto const& marker : project.markers) markers.push_back(toJson(marker));

    auto poseKeys = json::array();
    for (auto const& key : project.poseKeys) {
        poseKeys.push_back({
            {"id",           key.id          },
            {"subject",      key.subject     },
            {"subjectLabel", key.subjectLabel},
            {"tick",         key.tick        },
            {"animation",    key.animation   },
            {"hideItems",    key.hideItems   },
        });
    }

    auto videoTracks = json::array();
    for (auto const& track : project.videoTracks) videoTracks.push_back(toJson(track));

    auto transitions = json::array();
    for (auto const& transition : project.transitions) transitions.push_back(toJson(transition));

    auto globalPacks = json::array();
    for (auto const& pack : project.world.globalPacks) {
        globalPacks.push_back({
            {"id",      pack.id     },
            {"version", pack.version},
            {"name",    pack.name   },
        });
    }
    json const document{
        {"formatVersion",             kProjectFormatVersion            },
        {"projectName",               project.projectName              },
        {"replayPath",                project.projectPath              },
        {"totalTicks",                project.totalTicks               },
        {"currentTick",               project.currentTick              },
        {"fps",                       project.fps                      },
        {"activeVideoTrackIdx",       project.activeVideoTrackIdx      },
        {"cameras",                   std::move(cameras)               },
        {"worldActor",
         json{
             {"id", project.worldActor.id},
             {"name", project.worldActor.name},
             {"totalTicks", project.worldActor.totalTicks},
             {"segments", std::move(worldSegments)},
             {"subActors", std::move(subActors)}
         }                                                             },
        {"markers",                   std::move(markers)               },
        {"poseKeys",                  std::move(poseKeys)              },
        {"overlays",                  std::move(overlays)              },
        {"videoTracks",               std::move(videoTracks)           },
        {"transitions",               std::move(transitions)           },
        {"world",
         json{
             {"overrideTime", project.world.overrideTime},
             {"timeOfDay", project.world.timeOfDay},
             {"freezeTime", project.world.freezeTime},
             {"hideClouds", project.world.hideClouds},
             {"overrideFog", project.world.overrideFog},
             {"fogStart", project.world.fogStart},
             {"fogEnd", project.world.fogEnd},
             {"overrideFogColor", project.world.overrideFogColor},
             {"fogColor", project.world.fogColor},
             {"overrideWeather", project.world.overrideWeather},
             {"weather", static_cast<int>(project.world.weather)},
             {"overrideFov", project.world.overrideFov},
             {"fov", project.world.fov},
             {"overrideGraphics", project.world.overrideGraphics},
             {"graphics", static_cast<int>(project.world.graphics)},
             {"shaderOptions", project.world.shaderOptions},
             {"overrideGlobalPacks", project.world.overrideGlobalPacks},
             {"globalPacks", std::move(globalPacks)},
             {"overrideColor", project.world.overrideColor},
             {"exposure", project.world.exposure},
             {"temperature", project.world.temperature},
             {"tint", project.world.tint},
             {"channels", toJson(project.world.channels)}
         }                                                             },
        {"recordedCameraTracksAdded", project.recordedCameraTracksAdded}
    };
    return document.dump(2);
}

bool deserializeProject(std::string const& payload, model::EditorStateExt& project, std::string& error) {
    json document;
    try {
        document = json::parse(payload);
    } catch (std::exception const& e) {
        error = e.what();
        return false;
    }
    if (!document.is_object()) {
        error = "project file is not a JSON object";
        return false;
    }

    int const formatVersion = intOr(document, "formatVersion", 0);
    if (formatVersion <= 0 || formatVersion > kProjectFormatVersion) {
        error = "unsupported project format version " + std::to_string(formatVersion);
        return false;
    }

    model::EditorStateExt loaded;
    loaded.projectName         = stringOr(document, "projectName");
    loaded.projectPath         = stringOr(document, "replayPath");
    loaded.totalTicks          = intOr(document, "totalTicks", 0);
    loaded.currentTick         = intOr(document, "currentTick", 0);
    loaded.fps                 = finiteOr(document, "fps", 60.0f);
    loaded.activeVideoTrackIdx = intOr(document, "activeVideoTrackIdx", 0);

    if (auto const* cameras = arrayAt(document, "cameras")) {
        for (auto const& entry : *cameras) {
            if (entry.is_object()) loaded.cameras.push_back(cameraFrom(entry));
        }
    }
    if (auto const* worldActor = objectAt(document, "worldActor")) {
        loaded.worldActor.id         = stringOr(*worldActor, "id");
        loaded.worldActor.name       = stringOr(*worldActor, "name");
        loaded.worldActor.totalTicks = intOr(*worldActor, "totalTicks", loaded.totalTicks);
        if (auto const* segments = arrayAt(*worldActor, "segments")) {
            for (auto const& entry : *segments) {
                if (entry.is_object()) loaded.worldActor.segments.push_back(worldActorSegmentFrom(entry));
            }
        }
        if (auto const* subActors = arrayAt(*worldActor, "subActors")) {
            for (auto const& entry : *subActors) {
                if (entry.is_object()) loaded.worldActor.subActors.push_back(subActorFrom(entry));
            }
        }
    }
    if (auto const* markers = arrayAt(document, "markers")) {
        for (auto const& entry : *markers) {
            if (!entry.is_object()) continue;
            loaded.markers.push_back({stringOr(entry, "id"), stringOr(entry, "label"), intOr(entry, "tick", 0)});
        }
    }
    if (auto const* poseKeys = arrayAt(document, "poseKeys")) {
        for (auto const& entry : *poseKeys) {
            if (!entry.is_object()) continue;
            model::PoseKey key;
            key.id           = stringOr(entry, "id");
            key.subject      = stringOr(entry, "subject");
            key.subjectLabel = stringOr(entry, "subjectLabel");
            key.tick         = std::max(0, intOr(entry, "tick", 0));
            key.animation    = stringOr(entry, "animation");
            key.hideItems    = boolOr(entry, "hideItems", true);
            loaded.poseKeys.push_back(std::move(key));
        }
    }
    if (auto const* overlays = arrayAt(document, "overlays")) {
        for (auto const& entry : *overlays) {
            if (entry.is_object()) loaded.overlays.push_back(overlayFrom(entry));
        }
    }
    if (auto const* tracks = arrayAt(document, "videoTracks")) {
        for (auto const& entry : *tracks) {
            if (entry.is_object()) loaded.videoTracks.push_back(trackFrom(entry));
        }
    }
    if (auto const* transitions = arrayAt(document, "transitions")) {
        for (auto const& entry : *transitions) {
            if (entry.is_object()) loaded.transitions.push_back(transitionFrom(entry));
        }
    }
    if (auto const* world = objectAt(document, "world")) {
        model::WorldSettings const defaults;
        loaded.world.overrideTime = boolOr(*world, "overrideTime", defaults.overrideTime);
        loaded.world.timeOfDay    = intOr(*world, "timeOfDay", defaults.timeOfDay);
        loaded.world.freezeTime   = boolOr(*world, "freezeTime", defaults.freezeTime);
        loaded.world.hideClouds   = boolOr(*world, "hideClouds", defaults.hideClouds);
        loaded.world.overrideFog  = boolOr(*world, "overrideFog", defaults.overrideFog);
        loaded.world.fogStart     = finiteOr(*world, "fogStart", defaults.fogStart);
        loaded.world.fogEnd       = finiteOr(*world, "fogEnd", defaults.fogEnd);
        loaded.world.overrideFogColor = boolOr(*world, "overrideFogColor", defaults.overrideFogColor);
        loaded.world.fogColor         = defaults.fogColor;
        if (auto const* color = arrayAt(*world, "fogColor"); color && color->size() == 3) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if ((*color)[channel].is_number()) loaded.world.fogColor[channel] = (*color)[channel].get<float>();
            }
        }
        loaded.world.overrideWeather  = boolOr(*world, "overrideWeather", defaults.overrideWeather);
        loaded.world.weather          = enumOr(*world, "weather", defaults.weather, 2);
        loaded.world.overrideFov      = boolOr(*world, "overrideFov", defaults.overrideFov);
        loaded.world.fov              = finiteOr(*world, "fov", defaults.fov);
        loaded.world.overrideGraphics    = boolOr(*world, "overrideGraphics", defaults.overrideGraphics);
        loaded.world.graphics            = enumOr(*world, "graphics", defaults.graphics, 2);
        if (auto const* packs = objectAt(*world, "shaderOptions")) {
            for (auto const& [pack, options] : packs->items()) {
                if (!options.is_object()) continue;
                auto& values = loaded.world.shaderOptions[pack];
                for (auto const& [name, value] : options.items()) {
                    if (value.is_string()) values.emplace(name, value.get<std::string>());
                }
            }
        }
        loaded.world.overrideGlobalPacks = boolOr(*world, "overrideGlobalPacks", defaults.overrideGlobalPacks);
        if (auto const* packs = arrayAt(*world, "globalPacks")) {
            for (auto const& pack : *packs) {
                if (!pack.is_object()) continue;
                model::GlobalPack entry{stringOr(pack, "id"), stringOr(pack, "version"), stringOr(pack, "name")};
                if (!entry.id.empty()) loaded.world.globalPacks.push_back(std::move(entry));
            }
        }
        loaded.world.overrideColor = boolOr(*world, "overrideColor", defaults.overrideColor);
        loaded.world.exposure      = finiteOr(*world, "exposure", defaults.exposure);
        loaded.world.temperature   = finiteOr(*world, "temperature", defaults.temperature);
        loaded.world.tint          = finiteOr(*world, "tint", defaults.tint);
        loaded.world.channels      = channelsFrom(*world, "channels");
        loaded.world              = loaded.world.sanitized();
    }
    loaded.recordedCameraTracksAdded = boolOr(document, "recordedCameraTracksAdded", false);

    project = std::move(loaded);
    return true;
}

} // namespace playback::state::editing
