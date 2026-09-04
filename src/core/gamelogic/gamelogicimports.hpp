#pragma once

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::internal {

enum class GameLogicPeImportKind : std::uint8_t {
    named,
    ordinal,
    delay_named,
    delay_ordinal,
};

struct GameLogicPeImport {
    std::string module;
    GameLogicPeImportKind kind = GameLogicPeImportKind::named;
    std::string symbol;
    std::uint16_t ordinal = 0;
};

struct GameLogicImportReadResult {
    std::vector<GameLogicPeImport> imports;
    std::string unreadable_reason;

    bool readable() const noexcept { return unreadable_reason.empty(); }
};

struct GameLogicImportDiagnosis {
    std::string error_code;
    nlohmann::ordered_json error_fields = nlohmann::ordered_json::object();

    bool accepted() const noexcept { return error_code.empty(); }
};

inline constexpr std::string_view gameLogicAbiStaleErrorCode =
    "game_logic_abi_stale@1";
inline constexpr std::string_view gameLogicAbiUnsupportedImportErrorCode =
    "game_logic_abi_unsupported_import@1";
inline constexpr std::string_view gameLogicAbiUnreadableErrorCode =
    "game_logic_abi_unreadable@1";
inline constexpr std::string_view gameLogicLoadFailedErrorCode =
    "game_logic_load_failed@1";
inline constexpr std::string_view gameLogicRegistrationRejectedErrorCode =
    "game_logic_registration_rejected@1";
inline constexpr std::string_view gameLogicReloadFailedErrorCode =
    "game_logic_reload_failed@1";
inline constexpr std::string_view gameLogicUnavailableErrorCode =
    "game_logic_unavailable@1";
inline constexpr std::string_view gameLogicSourceBusyErrorCode =
    "game_logic_source_busy@1";
inline constexpr std::string_view gameLogicShadowCopyFailedErrorCode =
    "game_logic_shadow_copy_failed@1";
inline constexpr std::string_view gameLogicNotConfiguredErrorCode =
    "game_logic_not_configured@1";

// Reads only the top-level PE import and delay-import directories. It never
// maps or loads the image and does not follow imported modules.
GameLogicImportReadResult
readGameLogicImports(const std::filesystem::path &path) noexcept;

GameLogicImportDiagnosis diagnoseGameLogicImports(
    const std::filesystem::path &path, std::string_view host_module,
    std::uint32_t expected_epoch,
    std::uint32_t host_schema_entrance = 3) noexcept;

std::string_view gameLogicPeImportKindName(
    GameLogicPeImportKind kind) noexcept;

} // namespace Pelican::internal
