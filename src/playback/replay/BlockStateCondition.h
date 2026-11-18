#pragma once

#include <optional>
#include <string_view>

class CompoundTag;

namespace playback::replay {

// Evaluates a block permutation condition against one permutation's states (the "states" compound of its
// serialization ID). Covers the Molang add-ons write there: q./query.block_state('name'), number, string and
// true/false literals, ! and unary -, * / + -, < <= > >= == !=, && || and parentheses. nullopt when the expression
// uses anything else or names a state the permutation lacks, so the caller can leave that permutation alone.
[[nodiscard]] std::optional<bool> evaluateBlockStateCondition(std::string_view expression, CompoundTag const& states);

} // namespace playback::replay
