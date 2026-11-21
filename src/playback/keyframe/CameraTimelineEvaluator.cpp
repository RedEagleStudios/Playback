#include "CameraTimelineEvaluator.h"

#include "CameraKeyframeHandler.h"
#include "KeyframeTrack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <utility>

namespace playback::keyframe {

namespace {

using state::editing::model::CameraEntity;
using state::editing::model::CameraTrackKind;

bool isUsableCamera(CameraEntity const& camera) noexcept { return camera.enabled && !camera.keysByTick.empty(); }

bool isKeyframeCamera(CameraEntity const& camera) noexcept {
    return isUsableCamera(camera) && camera.kind == CameraTrackKind::Keyframe;
}

constexpr double TicksPerSecond = 20.0;

// lowbias32 (Chris Wellons): a cheap integer hash whose outputs for neighbouring inputs are unrelated.
uint32_t mixBits(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

// 0..1 from the top 24 bits, which a float holds exactly.
float unitFrom(uint32_t bits) { return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f); }

// 1D gradient (Perlin) noise in -1..1: a seeded slope at each whole cycle, blended with a quintic so the offset and
// its speed stay continuous. It passes through 0 at every whole cycle.
float gradientNoise(uint32_t seed, double cycles) {
    double const   cell     = std::floor(cycles);
    float const    fraction = static_cast<float>(cycles - cell);
    uint32_t const lattice  = static_cast<uint32_t>(static_cast<int64_t>(cell));
    auto const     slope    = [seed](uint32_t point) { return unitFrom(mixBits(seed ^ mixBits(point))) * 2.0f - 1.0f; };
    float const    from     = slope(lattice) * fraction;
    float const    to       = slope(lattice + 1U) * (fraction - 1.0f);
    float const    blend    = fraction * fraction * fraction * (fraction * (fraction * 6.0f - 15.0f) + 10.0f);
    return (from + (to - from) * blend) * 2.0f;
}

// One shake channel (an axis or an angle) in -1..1. A second, faster octave makes it read as a hand-held wobble
// rather than a slow swell, and each channel starts at its own offset so the axes never settle at the same moment.
float shakeNoise(uint32_t seed, uint32_t channel, double cycles) {
    uint32_t const channelSeed = mixBits(seed + (channel + 1U) * 0x9e3779b9U);
    double const   start       = cycles + unitFrom(mixBits(channelSeed ^ 0x5bd1e995U));
    return (gradientNoise(channelSeed, start) + 0.5f * gradientNoise(mixBits(channelSeed), start * 2.0)) / 1.5f;
}

bool coversTick(CameraEntity const& camera, int64_t tick) noexcept {
    return isUsableCamera(camera) && tick >= camera.keysByTick.begin()->first
        && tick <= camera.keysByTick.rbegin()->first;
}

std::string subjectAt(CameraEntity const& camera, int64_t tick) {
    auto key = camera.keysByTick.upper_bound(static_cast<int>(tick));
    if (key == camera.keysByTick.begin()) return {};
    return std::prev(key)->second.subject;
}

} // namespace

CameraTimelineEvaluator::CameraTimelineEvaluator(
    state::editing::model::EditorStateExt project,
    std::optional<std::string>            manualCameraId,
    bool                                  holdOutsideTracks,
    std::vector<int>                      dimensionTransitionTicks,
    SubjectViewSampler                    subjectSampler
)
: mProject(std::move(project)),
  mManualCameraId(std::move(manualCameraId)),
  mHoldOutsideTracks(holdOutsideTracks),
  mDimensionTransitionTicks(std::move(dimensionTransitionTicks)),
  mSubjectSampler(std::move(subjectSampler)) {
    std::erase_if(mDimensionTransitionTicks, [this](int tick) { return tick < 0 || tick > mProject.totalTicks; });
    std::sort(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end());
    mDimensionTransitionTicks.erase(
        std::unique(mDimensionTransitionTicks.begin(), mDimensionTransitionTicks.end()),
        mDimensionTransitionTicks.end()
    );

    for (auto& camera : mProject.cameras) {
        if (camera.keysByTick.empty() || camera.kind != CameraTrackKind::Keyframe) continue;

        std::vector<CameraTrackSegment> segments;
        KeyframeTrack::KeyMap           segmentKeys;
        size_t                          segmentIndex  = dimensionSegmentForTick(camera.keysByTick.begin()->first);
        auto                            appendSegment = [&] {
            if (segmentKeys.empty()) return;
            segments.emplace_back(
                CameraTrackSegment{
                    segmentIndex,
                    segmentKeys.begin()->first,
                    segmentKeys.rbegin()->first,
                    KeyframeTrack{segmentKeys},
                }
            );
            segmentKeys.clear();
        };

        for (auto const& [tick, keyframe] : camera.keysByTick) {
            auto const keySegment = dimensionSegmentForTick(tick);
            if (keySegment != segmentIndex) {
                appendSegment();
                segmentIndex = keySegment;
            }
            segmentKeys.emplace(tick, keyframe);
        }
        appendSegment();
        mKeyframeTracks.emplace(camera.id, std::move(segments));

        // Tracks that never shake skip the clock, and applyShake with them.
        using state::editing::model::CameraProperty::ShakeFrequency;
        using state::editing::model::CameraProperty::ShakeStrength;
        auto const strengthKeys = camera.channels.find(ShakeStrength);
        bool const shakes       = strengthKeys != camera.channels.end() && !strengthKeys->second.empty()
                                    ? std::ranges::any_of(strengthKeys->second, [](auto const& entry) {
                                          return entry.second.value > 0.0f;
                                      })
                                    : camera.shakeStrength > 0.0f;
        if (!shakes) continue;
        // The frequency's own keys, or one key holding its static value.
        state::editing::model::ValueChannel frequencyKeys;
        if (auto const keyed = camera.channels.find(ShakeFrequency);
            keyed != camera.channels.end() && !keyed->second.empty()) {
            frequencyKeys = keyed->second;
        } else {
            frequencyKeys.emplace(0, state::editing::model::ValueKey{camera.shakeFrequency, state::editing::model::KeyEasing::Hold});
        }
        std::vector<ShakeClockKey> clock;
        clock.reserve(frequencyKeys.size());
        for (auto const& [tick, keyframe] : frequencyKeys) {
            ShakeClockKey key{
                tick,
                state::editing::model::sanitizedShakeFrequency(keyframe.value),
                keyframe.easing == state::editing::model::KeyEasing::Hold,
            };
            if (!clock.empty()) {
                auto const&  previous = clock.back();
                double const seconds  = (tick - previous.tick) / TicksPerSecond;
                // A Hold key keeps its frequency to the next key; otherwise it moves linearly between them.
                double const frequency = previous.hold ? previous.frequency : (previous.frequency + key.frequency) * 0.5;
                key.cycles             = previous.cycles + seconds * frequency;
            }
            clock.push_back(key);
        }
        mShakeClocks.emplace(camera.id, std::move(clock));
    }
}

size_t CameraTimelineEvaluator::dimensionSegmentForTick(long double tick) const noexcept {
    auto const boundary = std::lower_bound(
        mDimensionTransitionTicks.begin(),
        mDimensionTransitionTicks.end(),
        tick,
        [](int transitionTick, long double value) { return static_cast<long double>(transitionTick) < value; }
    );
    return static_cast<size_t>(std::distance(mDimensionTransitionTicks.begin(), boundary));
}

std::optional<CameraRenderState>
CameraTimelineEvaluator::sampleKeyframes(CameraEntity const& camera, long double tick, bool clamp) const {
    auto const tracks = mKeyframeTracks.find(camera.id);
    if (tracks == mKeyframeTracks.end()) return std::nullopt;
    auto const segment =
        std::ranges::find(tracks->second, dimensionSegmentForTick(tick), &CameraTrackSegment::dimensionSegment);
    if (segment == tracks->second.end()) return std::nullopt;

    auto const keyTick = clamp ? std::clamp(
                                     tick,
                                     static_cast<long double>(segment->firstTick),
                                     static_cast<long double>(segment->lastTick)
                                 )
                               : tick;
    auto change = segment->track.createChange(keyTick);
    if (!change) return std::nullopt;

    CameraRenderStateHandler handler;
    change->apply(handler);
    auto state = handler.state();
    if (!state) return std::nullopt;
    double const time = static_cast<double>(tick);
    state->fov        = state::editing::model::cameraPropertyAt(camera, state::editing::model::CameraProperty::Fov, time);
    applyShake(
        *state,
        camera,
        state::editing::model::cameraPropertyAt(camera, state::editing::model::CameraProperty::ShakeStrength, time),
        tick
    );
    return state;
}

void CameraTimelineEvaluator::applyShake(
    CameraRenderState&  state,
    CameraEntity const& camera,
    float               strength,
    long double         tick
) const {
    strength = std::min(strength, state::editing::model::ShakeSettings::MaxStrength);
    // Not `<= 0`: a NaN strength must not shake either.
    if (!(strength > 0.0f)) return;
    auto const clock = mShakeClocks.find(camera.id);
    if (clock == mShakeClocks.end() || clock->second.empty()) return;

    // The clock's cycles at the key at or before the tick (the first key before the track), carried on to the tick.
    auto const&  keys    = clock->second;
    auto const   right   = std::ranges::upper_bound(keys, tick, std::less{}, [](ShakeClockKey const& key) {
        return static_cast<long double>(key.tick);
    });
    auto const&  left    = right == keys.begin() ? *right : *std::prev(right);
    double const seconds = static_cast<double>(tick - static_cast<long double>(left.tick)) / TicksPerSecond;
    double       cycles  = left.cycles + seconds * left.frequency;
    if (right != keys.begin() && right != keys.end() && !left.hold) {
        double const span  = (right->tick - left.tick) / TicksPerSecond;
        cycles            += seconds * seconds / span * (right->frequency - left.frequency) * 0.5;
    }

    // The amounts are the track's properties too, so a shake can grow or settle over time.
    using state::editing::model::cameraPropertyAt;
    namespace Property      = state::editing::model::CameraProperty;
    double const time       = static_cast<double>(tick);
    float const  position   = std::max(0.0f, cameraPropertyAt(camera, Property::ShakePosition, time));
    float const  rotation   = std::max(0.0f, cameraPropertyAt(camera, Property::ShakeRotation, time));
    float const  rollAmount = std::max(0.0f, cameraPropertyAt(camera, Property::ShakeRoll, time));
    uint32_t const seed     = camera.shake.seed;
    auto const offset       = [&](uint32_t channel, float amount) {
        return strength * amount * shakeNoise(seed, channel, cycles);
    };
    state.x     += offset(0, position);
    state.y     += offset(1, position);
    state.z     += offset(2, position);
    state.yaw   += offset(3, rotation);
    state.pitch += offset(4, rotation);
    state.roll  += offset(5, rollAmount);
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleTrack(CameraEntity const& camera, visuals::ReplaySampleTime const& time) const {
    if (camera.kind == CameraTrackKind::Keyframe) {
        auto state = sampleKeyframes(camera, time.value(), true);
        if (!state) return std::nullopt;
        return CameraTimelineEvaluation{*state, camera.id, camera.kind};
    }
    if (!mSubjectSampler) return std::nullopt;
    auto state = mSubjectSampler(time, subjectAt(camera, time.floorTick()), camera);
    if (!state) return std::nullopt;
    return CameraTimelineEvaluation{*state, camera.id, camera.kind};
}

CameraShot CameraTimelineEvaluator::shotAt(int64_t tick) const {
    for (auto const& camera : mProject.cameras) {
        if (!coversTick(camera, tick)) continue;
        if (camera.kind == CameraTrackKind::Keyframe) {
            // A keyframe track only owns the dimensions it has keys in.
            auto const tracks = mKeyframeTracks.find(camera.id);
            if (tracks == mKeyframeTracks.end()
                || std::ranges::find(
                       tracks->second,
                       dimensionSegmentForTick(static_cast<long double>(tick)),
                       &CameraTrackSegment::dimensionSegment
                   ) == tracks->second.end()) {
                continue;
            }
            return CameraShot{&camera, {}};
        }
        return CameraShot{&camera, subjectAt(camera, tick)};
    }
    return {};
}

std::optional<CameraTimelineEvaluation> CameraTimelineEvaluator::sample(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick = time.floorTick();

    for (auto const& camera : mProject.cameras) {
        if (!coversTick(camera, tick)) continue;
        // Without a sampler the caller views person tracks itself (the preview follows the subject natively).
        if (camera.kind != CameraTrackKind::Keyframe && !mSubjectSampler) return std::nullopt;
        // A track with nothing to show here (no keys in this dimension, subject absent) yields to lower ones.
        if (auto evaluation = sampleTrack(camera, time)) return evaluation;
    }

    if (mManualCameraId && !mManualCameraId->empty()) {
        if (auto evaluation = sampleCameraById(*mManualCameraId, time)) return evaluation;
    }
    if (!mHoldOutsideTracks) return std::nullopt;
    auto const fallback = std::ranges::find_if(mProject.cameras, isKeyframeCamera);
    if (fallback == mProject.cameras.end()) return std::nullopt;
    auto state = sampleKeyframes(*fallback, time.value(), true);
    return state ? std::optional<CameraTimelineEvaluation>{{*state, fallback->id, fallback->kind}} : std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleKeyframePath(visuals::ReplaySampleTime const& time) const {
    if (!time.isValid()) return std::nullopt;
    auto const tick = time.floorTick();
    for (auto const& camera : mProject.cameras) {
        if (camera.kind != CameraTrackKind::Keyframe || !coversTick(camera, tick)) continue;
        if (auto state = sampleKeyframes(camera, time.value(), true)) {
            return CameraTimelineEvaluation{*state, camera.id, camera.kind};
        }
    }
    return std::nullopt;
}

std::optional<CameraTimelineEvaluation>
CameraTimelineEvaluator::sampleCameraById(std::string_view cameraId, visuals::ReplaySampleTime const& time) const {
    if (cameraId.empty() || !time.isValid()) return std::nullopt;
    auto const camera = std::ranges::find(mProject.cameras, cameraId, &CameraEntity::id);
    if (camera == mProject.cameras.end() || !isKeyframeCamera(*camera)) return std::nullopt;
    auto state = sampleKeyframes(*camera, time.value(), true);
    return state ? std::optional<CameraTimelineEvaluation>{{*state, camera->id, camera->kind}} : std::nullopt;
}

} // namespace playback::keyframe
