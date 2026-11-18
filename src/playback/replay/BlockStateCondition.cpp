#include "BlockStateCondition.h"

#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/Int64Tag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ShortTag.h"
#include "mc/deps/nbt/StringTag.h"

#include <cctype>
#include <charconv>
#include <string>
#include <variant>

namespace playback::replay {

namespace {

using Value = std::variant<double, std::string>;

// Recursive descent over the condition text. Every parse step returns nullopt on anything outside the subset, which
// unwinds the whole evaluation.
class Evaluator {
public:
    Evaluator(std::string_view text, CompoundTag const& states) : mText(text), mStates(states) {}

    std::optional<bool> run() {
        auto const value = parseOr();
        skipSpace();
        if (!value || mPos != mText.size()) return std::nullopt;
        return truthy(*value);
    }

private:
    std::string_view   mText;
    CompoundTag const& mStates;
    size_t             mPos = 0;

    static bool truthy(Value const& value) {
        if (auto const* number = std::get_if<double>(&value)) return *number != 0.0;
        return !std::get<std::string>(value).empty();
    }

    void skipSpace() {
        while (mPos < mText.size() && std::isspace(static_cast<unsigned char>(mText[mPos]))) ++mPos;
    }

    bool accept(std::string_view token) {
        skipSpace();
        if (mText.substr(mPos, token.size()) != token) return false;
        mPos += token.size();
        return true;
    }

    // Binary operators that are prefixes of others (< of <=, ! of !=) are tried longest first by the callers.
    std::optional<Value> parseOr() {
        auto left = parseAnd();
        while (left && accept("||")) {
            auto right = parseAnd();
            if (!right) return std::nullopt;
            left = Value{truthy(*left) || truthy(*right) ? 1.0 : 0.0};
        }
        return left;
    }

    std::optional<Value> parseAnd() {
        auto left = parseEquality();
        while (left && accept("&&")) {
            auto right = parseEquality();
            if (!right) return std::nullopt;
            left = Value{truthy(*left) && truthy(*right) ? 1.0 : 0.0};
        }
        return left;
    }

    std::optional<Value> parseEquality() {
        auto left = parseRelational();
        while (left) {
            bool equal;
            if (accept("==")) equal = true;
            else if (accept("!=")) equal = false;
            else break;
            auto right = parseRelational();
            if (!right || left->index() != right->index()) return std::nullopt;
            left = Value{(*left == *right) == equal ? 1.0 : 0.0};
        }
        return left;
    }

    std::optional<Value> parseRelational() {
        auto left = parseAdditive();
        while (left) {
            std::string_view op;
            for (auto candidate : {"<=", ">=", "<", ">"}) {
                if (accept(candidate)) {
                    op = candidate;
                    break;
                }
            }
            if (op.empty()) break;
            auto right = parseAdditive();
            auto const* a = left ? std::get_if<double>(&*left) : nullptr;
            auto const* b = right ? std::get_if<double>(&*right) : nullptr;
            if (!a || !b) return std::nullopt;
            bool const result = op == "<=" ? *a <= *b : op == ">=" ? *a >= *b : op == "<" ? *a < *b : *a > *b;
            left              = Value{result ? 1.0 : 0.0};
        }
        return left;
    }

    std::optional<Value> parseAdditive() {
        auto left = parseMultiplicative();
        while (left) {
            char op;
            if (accept("+")) op = '+';
            else if (accept("-")) op = '-';
            else break;
            auto right = parseMultiplicative();
            auto const* a = std::get_if<double>(&*left);
            auto const* b = right ? std::get_if<double>(&*right) : nullptr;
            if (!a || !b) return std::nullopt;
            left = Value{op == '+' ? *a + *b : *a - *b};
        }
        return left;
    }

    std::optional<Value> parseMultiplicative() {
        auto left = parseUnary();
        while (left) {
            char op;
            if (accept("*")) op = '*';
            else if (accept("/")) op = '/';
            else break;
            auto right = parseUnary();
            auto const* a = std::get_if<double>(&*left);
            auto const* b = right ? std::get_if<double>(&*right) : nullptr;
            if (!a || !b || (op == '/' && *b == 0.0)) return std::nullopt;
            left = Value{op == '*' ? *a * *b : *a / *b};
        }
        return left;
    }

    std::optional<Value> parseUnary() {
        skipSpace();
        if (mPos < mText.size() && mText[mPos] == '!' && mText.substr(mPos, 2) != "!=") {
            ++mPos;
            auto operand = parseUnary();
            if (!operand) return std::nullopt;
            return Value{truthy(*operand) ? 0.0 : 1.0};
        }
        if (accept("-")) {
            auto operand = parseUnary();
            auto const* number = operand ? std::get_if<double>(&*operand) : nullptr;
            if (!number) return std::nullopt;
            return Value{-*number};
        }
        return parsePrimary();
    }

    std::optional<std::string> parseString() {
        skipSpace();
        if (mPos >= mText.size() || (mText[mPos] != '\'' && mText[mPos] != '"')) return std::nullopt;
        char const quote = mText[mPos++];
        auto const end   = mText.find(quote, mPos);
        if (end == std::string_view::npos) return std::nullopt;
        std::string text(mText.substr(mPos, end - mPos));
        mPos = end + 1;
        return text;
    }

    std::optional<Value> parsePrimary() {
        skipSpace();
        if (mPos >= mText.size()) return std::nullopt;
        char const next = mText[mPos];
        if (next == '(') {
            ++mPos;
            auto inner = parseOr();
            if (!inner || !accept(")")) return std::nullopt;
            return inner;
        }
        if (next == '\'' || next == '"') {
            auto text = parseString();
            if (!text) return std::nullopt;
            return Value{std::move(*text)};
        }
        if (std::isdigit(static_cast<unsigned char>(next)) || next == '.') {
            double     number{};
            auto const begin  = mText.data() + mPos;
            auto const result = std::from_chars(begin, mText.data() + mText.size(), number);
            if (result.ec != std::errc{}) return std::nullopt;
            mPos += static_cast<size_t>(result.ptr - begin);
            return Value{number};
        }

        size_t const start = mPos;
        while (mPos < mText.size()
               && (std::isalnum(static_cast<unsigned char>(mText[mPos])) || mText[mPos] == '_' || mText[mPos] == '.')) {
            ++mPos;
        }
        std::string identifier(mText.substr(start, mPos - start));
        for (auto& character : identifier) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (identifier == "true") return Value{1.0};
        if (identifier == "false") return Value{0.0};
        if (identifier != "q.block_state" && identifier != "query.block_state") return std::nullopt;

        if (!accept("(")) return std::nullopt;
        auto name = parseString();
        if (!name || !accept(")")) return std::nullopt;
        return stateValue(*name);
    }

    std::optional<Value> stateValue(std::string const& name) const {
        if (!mStates.contains(name)) return std::nullopt;
        auto const& state = mStates.at(name);
        if (state.hold<StringTag>()) return Value{static_cast<std::string const&>(state.get<StringTag>())};
        if (state.hold<IntTag>()) return Value{static_cast<double>(state.get<IntTag>().data)};
        if (state.hold<ByteTag>()) return Value{static_cast<double>(state.get<ByteTag>().data)};
        if (state.hold<ShortTag>()) return Value{static_cast<double>(state.get<ShortTag>().data)};
        if (state.hold<Int64Tag>()) return Value{static_cast<double>(state.get<Int64Tag>().data)};
        return std::nullopt;
    }
};

} // namespace

std::optional<bool> evaluateBlockStateCondition(std::string_view expression, CompoundTag const& states) {
    return Evaluator(expression, states).run();
}

} // namespace playback::replay
