#include "ClientVisualOverrides.h"

#include "playback/Playback.h"
#include "playback/io/ReplayResourcePacks.h"

#include "ll/api/service/Bedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/deps/core/file/PathBuffer.h"
#include "mc/deps/core/resource/PackIdVersion.h"
#include "mc/deps/core/resource/PackOrigin.h"
#include "mc/deps/core/resource/PackType.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/resource/ResourcePackStackType.h"
#include "mc/deps/core/sem_ver/SemVersion.h"
#include "mc/deps/core/utility/NonOwnerPointer.h"
#include "mc/options/option_types/IntOption.h"
#include "mc/options/option_types/OptionID.h"
#include "mc/platform/UUID.h"
#include "mc/resources/IResourcePackRepository.h"
#include "mc/resources/ResourcePack.h"
#include "mc/resources/ResourcePackManager.h"
#include "mc/resources/ResourcePackStack.h"
#include "mc/scripting/modules/PackManifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace playback::replay {

namespace {

namespace model = state::editing::model;

auto& logger() { return Playback::getInstance().getSelf().getLogger(); }

::IntOption* graphicsModeOption(ClientInstance& client) {
    auto const option = client.getOptions().getIfValid(::OptionID::GraphicsMode);
    return option ? static_cast<::IntOption*>(*option) : nullptr;
}

// "1.2.3" as global_resource_packs.json writes versions: [1, 2, 3].
nlohmann::json versionArray(std::string_view version) {
    auto parts = nlohmann::json::array();
    for (auto const part : std::views::split(version, '.')) {
        int value{};
        std::from_chars(part.data(), part.data() + part.size(), value);
        parts.push_back(value);
        if (parts.size() == 3) break;
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

// Packs a viewer installed or develops; built-in (Package), world, cache and base-game packs are not theirs to pick.
bool isGlobalPackCandidate(::PackOrigin origin) { return origin == ::PackOrigin::User || origin == ::PackOrigin::Dev; }

// Packs that name themselves through their own lang files hold the key "pack.name"; their folder reads better.
std::string displayName(::PackManifest const& manifest) {
    std::string const& name = *manifest.mName;
    if (!name.empty() && name != "pack.name") return name;
    std::string_view path = manifest.mLocation->mPath->value;
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
    auto const slash  = path.find_last_of("/\\");
    auto const folder = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return folder.empty() ? name : std::string(folder);
}

} // namespace

ClientVisualOverrides::ClientVisualOverrides()  = default;
ClientVisualOverrides::~ClientVisualOverrides() = default;

void ClientVisualOverrides::apply() {
    if (!mInstalledPacks) readInstalledPacks();
    if (!mGraphics) {
        restoreGraphics();
        return;
    }
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto const option = graphicsModeOption(*client);
    if (!option) return;
    if (!mSavedGraphics) mSavedGraphics = option->mValue;
    int const mode = static_cast<int>(*mGraphics);
    if (option->mValue != mode) option->set(mode, false);
}

void ClientVisualOverrides::restore() {
    restoreGraphics();
    restoreGlobalPacks();
    mGraphics.reset();
    mInstalledPacks.reset();
}

void ClientVisualOverrides::restoreGraphics() {
    auto const saved = std::exchange(mSavedGraphics, std::nullopt);
    if (!saved) return;
    auto client = ll::service::getClientInstance();
    if (!client) return;
    if (auto const option = graphicsModeOption(*client); option && option->mValue != *saved) option->set(*saved, false);
}

void ClientVisualOverrides::useGlobalPacks(std::optional<std::vector<model::GlobalPack>> const& packs) {
    restoreGlobalPacks();
    if (!packs) return;
    auto client = ll::service::getClientInstance();
    if (!client) return;
    auto&                                                              manager = client->getResourcePackManager();
    Bedrock::NotNullNonOwnerPtr<::IResourcePackRepository const> const repo{
        Bedrock::NonOwnerPointer<::IResourcePackRepository const>(client->getResourcePackRepository())
    };

    auto entries = nlohmann::json::array();
    for (auto const& pack : *packs) {
        entries.push_back({
            {"pack_id", pack.id                   },
            {"version", versionArray(pack.version)},
        });
    }
    auto stack = ::ResourcePackStack::deserialize(entries.dump(), repo, std::nullopt);
    // The viewer's packs move into a stack of ours rather than the game's stack being taken: the game's stack object
    // must never be null. The vector swap keeps their subpack choices without copying anything.
    auto saved = ::ResourcePackStack::deserialize("[]", repo, std::nullopt);
    if (!stack || !saved) {
        logger().warn("Unable to build the project's global resource pack stack; the viewer's packs stay");
        return;
    }
    if (auto const& current = manager.mGlobalStack) std::swap(*saved->mStack, *current->mStack);
    mSavedGlobalStack = std::move(saved);
    manager.setStack(std::move(stack), ::ResourcePackStackType::Global);
    logger().info("Replay uses {} project global resource pack(s)", packs->size());
}

void ClientVisualOverrides::restoreGlobalPacks() {
    auto saved = std::move(mSavedGlobalStack);
    if (!saved) return;
    auto client = ll::service::getClientInstance();
    if (!client) return;
    client->getResourcePackManager().setStack(std::move(saved), ::ResourcePackStackType::Global);
    logger().info("Restored the viewer's global resource packs");
}

void ClientVisualOverrides::readInstalledPacks() {
    auto client = ll::service::getClientInstance();
    if (!client) return;
    std::vector<model::GlobalPack> packs;
    client->getResourcePackRepository().forEachPack([&](::ResourcePack const& pack) {
        auto const& manifest = io::packs::manifestOf(pack);
        if (manifest.mPackType != ::PackType::Resources || !isGlobalPackCandidate(manifest.mPackOrigin)) return;
        model::GlobalPack entry{
            manifest.mIdentity->mId->asString(),
            manifest.mIdentity->mVersion->asString(),
            displayName(manifest),
        };
        if (std::ranges::find(packs, entry) == packs.end()) packs.push_back(std::move(entry));
    });
    std::ranges::sort(packs, {}, &model::GlobalPack::name);
    mInstalledPacks = std::make_shared<std::vector<model::GlobalPack> const>(std::move(packs));
}

} // namespace playback::replay
