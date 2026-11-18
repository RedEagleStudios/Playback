#include "playback/integration/ShaderPack.h"

#include "zip.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

namespace playback::integration {

namespace {

std::string_view trim(std::string_view text) {
    auto const first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    auto const last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool isNameChar(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::istringstream       stream{std::string(text)};
    for (std::string word; stream >> word;) out.push_back(std::move(word));
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

// The bracketed value list of an option comment, `// Label [a b c]`; empty when there is none.
std::vector<std::string> listedValues(std::string_view comment) {
    auto const open  = comment.find('[');
    auto const close = comment.find(']', open == std::string_view::npos ? 0 : open);
    if (open == std::string_view::npos || close == std::string_view::npos) return {};
    return words(comment.substr(open + 1, close - open - 1));
}

void addOption(ShaderPackInfo& pack, ShaderOption option) {
    // The first declaration wins, as in Iris.
    pack.options.try_emplace(option.name, std::move(option));
}

// `#define NAME`, `//#define NAME`, `#define NAME value // [..]` and `const type name = value; // [..]`.
void readSourceLine(ShaderPackInfo& pack, std::string_view line) {
    line           = trim(line);
    bool commented = false;
    if (line.starts_with("//")) {
        commented = true;
        line      = trim(line.substr(2));
    }
    if (line.starts_with("#define")) {
        auto rest = trim(line.substr(7));
        auto end  = std::size_t{0};
        while (end < rest.size() && isNameChar(rest[end])) ++end;
        if (end == 0) return;
        ShaderOption option;
        option.name = std::string(rest.substr(0, end));
        auto tail   = trim(rest.substr(end));
        if (tail.empty() || tail.starts_with("//")) {
            option.toggle       = true;
            option.defaultValue = commented ? "false" : "true";
            addOption(pack, std::move(option));
            return;
        }
        if (commented) return;
        auto const commentAt = tail.find("//");
        if (commentAt == std::string_view::npos) return;
        auto const value = trim(tail.substr(0, commentAt));
        option.values    = listedValues(tail.substr(commentAt + 2));
        if (value.empty() || option.values.empty()) return;
        option.defaultValue = std::string(value);
        addOption(pack, std::move(option));
        return;
    }
    if (commented || !line.starts_with("const ")) return;
    auto const equals    = line.find('=');
    auto const semicolon = line.find(';');
    auto const commentAt = line.find("//");
    if (equals == std::string_view::npos || semicolon == std::string_view::npos || commentAt == std::string_view::npos
        || semicolon < equals || commentAt < semicolon) {
        return;
    }
    auto const declaration = words(line.substr(0, equals));
    if (declaration.size() != 3) return;
    ShaderOption option;
    option.name         = declaration[2];
    option.defaultValue = std::string(trim(line.substr(equals + 1, semicolon - equals - 1)));
    option.values       = listedValues(line.substr(commentAt + 2));
    if (option.values.empty()) return;
    addOption(pack, std::move(option));
}

void readSource(ShaderPackInfo& pack, std::string_view text) {
    while (!text.empty()) {
        auto const end = text.find('\n');
        readSourceLine(pack, text.substr(0, end));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
}

// Lines of a .properties or .lang file, with `\` continuations joined.
std::vector<std::pair<std::string, std::string>> keyValues(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string                                      pending;
    auto const                                       flush = [&] {
        auto const line = trim(pending);
        if (!line.empty() && !line.starts_with('#')) {
            if (auto const equals = line.find('='); equals != std::string_view::npos) {
                out.emplace_back(std::string(trim(line.substr(0, equals))), std::string(trim(line.substr(equals + 1))));
            }
        }
        pending.clear();
    };
    while (!text.empty()) {
        auto const end  = text.find('\n');
        auto       line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line.back() == '\\') {
            pending.append(line.substr(0, line.size() - 1)).push_back(' ');
        } else {
            pending.append(line);
            flush();
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    if (!pending.empty()) flush();
    return out;
}

void readProperties(ShaderPackInfo& pack, std::string_view text) {
    std::set<std::string> sliders;
    for (auto const& [key, value] : keyValues(text)) {
        if (key == "sliders") {
            for (auto& name : words(value)) sliders.insert(std::move(name));
        } else if (key == "screen") {
            pack.screens[""] = words(value);
        } else if (key.starts_with("screen.") && key.find('.', 7) == std::string::npos) {
            pack.screens[key.substr(7)] = words(value);
        }
    }
    for (auto& [name, option] : pack.options) option.slider = sliders.contains(name);
}

void readLang(ShaderPackInfo& pack, std::string_view text) {
    for (auto& [key, value] : keyValues(text)) pack.lang.insert_or_assign(std::move(key), std::move(value));
}

bool isSourceFile(std::string_view name) {
    auto const dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    auto const extension = lower(name.substr(dot));
    return extension == ".glsl" || extension == ".fsh" || extension == ".vsh" || extension == ".gsh"
        || extension == ".csh" || extension == ".inc";
}

std::optional<std::string> readFile(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// Every file under the pack's `shaders/` folder, by its path below that folder with `/` separators.
std::optional<std::map<std::string, std::string>> packFiles(std::filesystem::path const& pack) {
    std::map<std::string, std::string> files;
    std::error_code                    error;
    if (std::filesystem::is_directory(pack, error)) {
        auto const root = pack / "shaders";
        for (auto const& entry : std::filesystem::recursive_directory_iterator(root, error)) {
            if (!entry.is_regular_file(error)) continue;
            auto relative = entry.path().lexically_relative(root).generic_u8string();
            auto name     = std::string(relative.begin(), relative.end());
            if (auto text = readFile(entry.path())) files.emplace(std::move(name), std::move(*text));
        }
        return files;
    }

    int  zipError = 0;
    auto path     = pack.u8string();
    std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
        zip_open(reinterpret_cast<char const*>(path.c_str()), ZIP_RDONLY, &zipError),
        &zip_discard
    );
    if (!archive) return std::nullopt;
    auto const count = zip_get_num_entries(archive.get(), 0);
    // Packs zipped with their own folder on top keep `shaders/` one level down.
    std::string prefix = "shaders/";
    for (zip_int64_t index = 0; index < count; ++index) {
        auto const* raw = zip_get_name(archive.get(), static_cast<zip_uint64_t>(index), 0);
        if (!raw) continue;
        std::string_view const name = raw;
        if (name.ends_with("shaders/shaders.properties")) {
            prefix = std::string(name.substr(0, name.size() - std::string_view("shaders.properties").size()));
            break;
        }
    }
    for (zip_int64_t index = 0; index < count; ++index) {
        auto const* raw = zip_get_name(archive.get(), static_cast<zip_uint64_t>(index), 0);
        if (!raw) continue;
        std::string_view const name = raw;
        if (!name.starts_with(prefix) || name.ends_with('/')) continue;
        zip_stat_t stat;
        zip_stat_init(&stat);
        if (zip_stat_index(archive.get(), static_cast<zip_uint64_t>(index), 0, &stat) != 0
            || (stat.valid & ZIP_STAT_SIZE) == 0) {
            continue;
        }
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(
            zip_fopen_index(archive.get(), static_cast<zip_uint64_t>(index), 0),
            &zip_fclose
        );
        if (!file) continue;
        std::string text(static_cast<std::size_t>(stat.size), '\0');
        if (zip_fread(file.get(), text.data(), text.size()) != static_cast<zip_int64_t>(text.size())) continue;
        files.emplace(std::string(name.substr(prefix.size())), std::move(text));
    }
    return files;
}

std::optional<float> number(std::string_view text) {
    float value{};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::string ShaderPackInfo::text(std::string const& key, std::string_view fallback) const {
    auto const found = lang.find(key);
    if (found == lang.end() || found->second.empty()) return std::string(fallback);
    // Minecraft formatting codes (§ and one character) colour the pack's own menu; they mean nothing here.
    std::string out;
    std::string_view const source = found->second;
    for (std::size_t at = 0; at < source.size(); ++at) {
        if (source.substr(at).starts_with("\xC2\xA7")) {
            at += 2;
            continue;
        }
        out.push_back(source[at]);
    }
    return out;
}

ShaderOption const* ShaderPackInfo::option(std::string const& name) const {
    auto const found = options.find(name);
    return found == options.end() ? nullptr : &found->second;
}

std::optional<ShaderPackInfo> readShaderPack(std::filesystem::path const& pack) {
    auto const files = packFiles(pack);
    if (!files) return std::nullopt;
    ShaderPackInfo info;
    for (auto const& [name, text] : *files) {
        if (isSourceFile(name)) readSource(info, text);
    }
    if (auto const properties = files->find("shaders.properties"); properties != files->end()) {
        readProperties(info, properties->second);
    }
    for (auto const& [name, text] : *files) {
        if (lower(name) == "lang/en_us.lang") readLang(info, text);
    }
    return info;
}

std::string nearestShaderValue(ShaderOption const& option, float value) {
    std::string nearest;
    float       distance = INFINITY;
    for (auto const& candidate : option.values) {
        auto const parsed = number(candidate);
        if (!parsed || std::abs(*parsed - value) >= distance) continue;
        distance = std::abs(*parsed - value);
        nearest  = candidate;
    }
    return nearest;
}

} // namespace playback::integration
