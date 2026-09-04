#include "gamelogicimports.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <system_error>
#include <vector>

namespace Pelican::internal {
namespace {

constexpr std::uint16_t pe32Magic = 0x10b;
constexpr std::uint16_t pe32PlusMagic = 0x20b;
constexpr std::size_t importDirectoryIndex = 1;
constexpr std::size_t delayImportDirectoryIndex = 13;
// These are parser work limits, not PE format limits.  The reader is a
// pre-load diagnostic and must never let a malformed input turn into an
// unbounded allocation or scan.
constexpr std::uint64_t maxPeFileBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t maxImportDescriptors = 4096;
constexpr std::size_t maxImportThunks = 65536;

struct DataDirectory {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

struct Section {
    std::uint32_t virtual_size = 0;
    std::uint32_t virtual_address = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t raw_offset = 0;
};

std::string asciiLower(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto ch : value) {
        result.push_back(ch >= 'A' && ch <= 'Z'
                             ? static_cast<char>(ch - 'A' + 'a')
                             : ch);
    }
    return result;
}

class PeReader {
    std::ifstream &input_;
    std::uint64_t file_size_ = 0;
    std::vector<Section> sections_;
    std::uint32_t size_of_headers_ = 0;
    std::uint64_t image_base_ = 0;
    bool pe32_plus_ = false;

  public:
    explicit PeReader(std::ifstream &input, std::uint64_t file_size)
        : input_{input}, file_size_{file_size} {}

    template <class Value>
    std::optional<Value> readValue(std::size_t offset) const noexcept {
        if (offset > file_size_ || sizeof(Value) > file_size_ - offset) {
            return std::nullopt;
        }
        Value value{};
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(offset));
        if (!input_.read(reinterpret_cast<char *>(&value), sizeof(Value))) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::string> readCString(std::size_t offset) const {
        constexpr std::size_t maxImportStringBytes = 4096;
        if (offset >= file_size_) return std::nullopt;
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(
            maxImportStringBytes, file_size_ - offset));
        std::array<char, maxImportStringBytes> value{};
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(offset));
        if (!input_.read(value.data(), static_cast<std::streamsize>(available))) {
            return std::nullopt;
        }
        const auto *end = static_cast<const char *>(
            std::memchr(value.data(), '\0', available));
        if (end == nullptr) return std::nullopt;
        return std::string(value.data(), static_cast<std::size_t>(end - value.data()));
    }

    bool parseHeaders(DataDirectory &imports, DataDirectory &delay_imports,
                      std::string &reason) {
        const auto mz = readValue<std::uint16_t>(0);
        if (!mz || *mz != 0x5a4d) {
            reason = "not_pe";
            return false;
        }
        const auto pe_offset = readValue<std::uint32_t>(0x3c);
        if (!pe_offset || *pe_offset > file_size_ ||
            file_size_ - *pe_offset < 24) {
            reason = "truncated_pe_header";
            return false;
        }
        const auto signature = readValue<std::uint32_t>(*pe_offset);
        if (!signature || *signature != 0x00004550) {
            reason = "invalid_pe_signature";
            return false;
        }
        const auto coff = static_cast<std::size_t>(*pe_offset) + 4;
        const auto section_count = readValue<std::uint16_t>(coff + 2);
        const auto optional_size = readValue<std::uint16_t>(coff + 16);
        if (!section_count || !optional_size) {
            reason = "truncated_coff_header";
            return false;
        }
        const auto optional = coff + 20;
        if (optional > file_size_ || file_size_ - optional < *optional_size) {
            reason = "truncated_optional_header";
            return false;
        }
        const auto magic = readValue<std::uint16_t>(optional);
        if (!magic || (*magic != pe32Magic && *magic != pe32PlusMagic)) {
            reason = "unsupported_optional_header";
            return false;
        }
        pe32_plus_ = *magic == pe32PlusMagic;
        const std::size_t number_offset = pe32_plus_ ? 108 : 92;
        const std::size_t directories_offset = pe32_plus_ ? 112 : 96;
        if (*optional_size < directories_offset) {
            reason = "truncated_data_directories";
            return false;
        }
        const auto directory_count =
            readValue<std::uint32_t>(optional + number_offset);
        const auto size_of_headers =
            readValue<std::uint32_t>(optional + 60);
        if (!directory_count || !size_of_headers) {
            reason = "truncated_optional_header";
            return false;
        }
        size_of_headers_ = *size_of_headers;
        if (pe32_plus_) {
            const auto image_base =
                readValue<std::uint64_t>(optional + 24);
            if (!image_base) {
                reason = "truncated_image_base";
                return false;
            }
            image_base_ = *image_base;
        } else {
            const auto image_base =
                readValue<std::uint32_t>(optional + 28);
            if (!image_base) {
                reason = "truncated_image_base";
                return false;
            }
            image_base_ = *image_base;
        }

        const auto read_directory = [&](std::size_t index,
                                        DataDirectory &directory) {
            if (*directory_count <= index) return true;
            const auto entry = optional + directories_offset + index * 8;
            if (entry > optional + *optional_size ||
                optional + *optional_size - entry < 8) {
                return false;
            }
            const auto rva = readValue<std::uint32_t>(entry);
            const auto size = readValue<std::uint32_t>(entry + 4);
            if (!rva || !size) return false;
            directory = {*rva, *size};
            return true;
        };
        if (!read_directory(importDirectoryIndex, imports) ||
            !read_directory(delayImportDirectoryIndex, delay_imports)) {
            reason = "truncated_data_directories";
            return false;
        }

        const auto section_table = optional + *optional_size;
        if (section_table > file_size_ ||
            static_cast<std::size_t>(*section_count) >
                (file_size_ - section_table) / 40) {
            reason = "truncated_section_table";
            return false;
        }
        sections_.reserve(*section_count);
        for (std::size_t index = 0; index < *section_count; ++index) {
            const auto offset = section_table + index * 40;
            const auto virtual_size =
                readValue<std::uint32_t>(offset + 8);
            const auto virtual_address =
                readValue<std::uint32_t>(offset + 12);
            const auto raw_size =
                readValue<std::uint32_t>(offset + 16);
            const auto raw_offset =
                readValue<std::uint32_t>(offset + 20);
            if (!virtual_size || !virtual_address || !raw_size ||
                !raw_offset) {
                reason = "truncated_section_table";
                return false;
            }
            sections_.push_back(
                {*virtual_size, *virtual_address, *raw_size, *raw_offset});
        }
        return true;
    }

    std::optional<std::size_t> fileOffset(std::uint32_t rva,
                                          std::size_t required = 1) const {
        if (rva < size_of_headers_) {
            const auto offset = static_cast<std::size_t>(rva);
            if (offset <= file_size_ && required <= file_size_ - offset) {
                return offset;
            }
            return std::nullopt;
        }
        for (const auto &section : sections_) {
            const auto span = std::max(section.virtual_size, section.raw_size);
            if (rva < section.virtual_address ||
                static_cast<std::uint64_t>(rva) >=
                    static_cast<std::uint64_t>(section.virtual_address) + span) {
                continue;
            }
            const auto delta = rva - section.virtual_address;
            if (delta > section.raw_size ||
                required > static_cast<std::size_t>(section.raw_size - delta)) {
                return std::nullopt;
            }
            const auto offset =
                static_cast<std::uint64_t>(section.raw_offset) + delta;
            if (offset > file_size_ ||
                required > file_size_ - static_cast<std::size_t>(offset)) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(offset);
        }
        return std::nullopt;
    }

    // Import descriptors form a directory-local array in virtual memory.  A
    // descriptor may not be assembled by walking raw file bytes into a later
    // section: that can otherwise make an attacker-controlled descriptor look
    // valid after a section boundary.
    std::optional<std::size_t> directoryDescriptorOffset(
        const DataDirectory &directory, std::size_t index,
        std::size_t descriptor_size) const {
        if (index >= maxImportDescriptors ||
            index > std::numeric_limits<std::size_t>::max() / descriptor_size ||
            index * descriptor_size > directory.size ||
            descriptor_size > directory.size - index * descriptor_size) {
            return std::nullopt;
        }
        const auto rva64 = static_cast<std::uint64_t>(directory.rva) +
                           index * descriptor_size;
        if (rva64 > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        const auto descriptor_rva = static_cast<std::uint32_t>(rva64);
        const auto directory_section = sectionForRva(directory.rva, 1);
        const auto descriptor_section =
            sectionForRva(descriptor_rva, descriptor_size);
        if (!directory_section || !descriptor_section ||
            *directory_section != *descriptor_section) {
            return std::nullopt;
        }
        return fileOffset(descriptor_rva, descriptor_size);
    }

    std::optional<std::size_t> sectionForRva(std::uint32_t rva,
                                             std::size_t required) const {
        for (std::size_t index = 0; index < sections_.size(); ++index) {
            const auto &section = sections_[index];
            const auto span = std::max(section.virtual_size, section.raw_size);
            if (rva < section.virtual_address ||
                static_cast<std::uint64_t>(rva) >=
                    static_cast<std::uint64_t>(section.virtual_address) + span) {
                continue;
            }
            const auto delta = rva - section.virtual_address;
            if (delta <= section.raw_size &&
                required <= static_cast<std::size_t>(section.raw_size - delta)) {
                return index;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> delayFieldRva(std::uint32_t value,
                                               bool fields_are_rvas) const {
        if (value == 0) return std::uint32_t{0};
        if (fields_are_rvas) return value;
        if (static_cast<std::uint64_t>(value) < image_base_ ||
            static_cast<std::uint64_t>(value) - image_base_ >
                std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(value) - image_base_);
    }

    bool appendThunkImports(std::uint32_t thunk_rva, std::string module,
                            bool delay,
                            std::vector<GameLogicPeImport> &output,
                            std::string &reason) const {
        if (thunk_rva == 0) return true;
        const std::size_t width = pe32_plus_ ? 8 : 4;
        const std::uint64_t ordinal_flag =
            pe32_plus_ ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
        for (std::uint64_t index = 0; index < maxImportThunks; ++index) {
            const auto entry_rva = static_cast<std::uint64_t>(thunk_rva) +
                                   index * width;
            if (entry_rva > std::numeric_limits<std::uint32_t>::max()) {
                reason = "invalid_import_thunk";
                return false;
            }
            const auto entry_offset =
                fileOffset(static_cast<std::uint32_t>(entry_rva), width);
            if (!entry_offset) {
                reason = "truncated_import_thunk";
                return false;
            }
            std::uint64_t value = 0;
            if (pe32_plus_) {
                const auto entry = readValue<std::uint64_t>(*entry_offset);
                if (!entry) {
                    reason = "truncated_import_thunk";
                    return false;
                }
                value = *entry;
            } else {
                const auto entry = readValue<std::uint32_t>(*entry_offset);
                if (!entry) {
                    reason = "truncated_import_thunk";
                    return false;
                }
                value = *entry;
            }
            if (value == 0) return true;
            if ((value & ordinal_flag) != 0) {
                output.push_back(GameLogicPeImport{
                    .module = module,
                    .kind = delay ? GameLogicPeImportKind::delay_ordinal
                                  : GameLogicPeImportKind::ordinal,
                    .ordinal = static_cast<std::uint16_t>(value & 0xffffU),
                });
                continue;
            }
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                reason = "invalid_import_name_rva";
                return false;
            }
            const auto name_offset =
                fileOffset(static_cast<std::uint32_t>(value), 3);
            if (!name_offset) {
                reason = "truncated_import_name";
                return false;
            }
            const auto name = readCString(*name_offset + 2);
            if (!name) {
                reason = "unterminated_import_name";
                return false;
            }
            output.push_back(GameLogicPeImport{
                .module = module,
                .kind = delay ? GameLogicPeImportKind::delay_named
                              : GameLogicPeImportKind::named,
                .symbol = *name,
            });
        }
        reason = "too_large";
        return false;
    }

    bool appendNormalImports(const DataDirectory &directory,
                             std::vector<GameLogicPeImport> &output,
                             std::string &reason) const {
        if (directory.rva == 0 || directory.size == 0) return true;
        if (directory.size / 20 > maxImportDescriptors) {
            reason = "too_large";
            return false;
        }
        for (std::size_t index = 0; index * 20 + 20 <= directory.size; ++index) {
            const auto offset = directoryDescriptorOffset(directory, index, 20);
            if (!offset) {
                reason = "descriptor_out_of_bounds";
                return false;
            }
            const auto original_thunk = readValue<std::uint32_t>(*offset);
            const auto name_rva = readValue<std::uint32_t>(*offset + 12);
            const auto first_thunk = readValue<std::uint32_t>(*offset + 16);
            if (!original_thunk || !name_rva || !first_thunk) {
                reason = "truncated_import_directory";
                return false;
            }
            if (*original_thunk == 0 && *name_rva == 0 && *first_thunk == 0) {
                return true;
            }
            const auto name_offset = fileOffset(*name_rva);
            const auto module = name_offset
                                    ? readCString(*name_offset)
                                    : std::optional<std::string>{};
            if (!module) {
                reason = "invalid_import_module_name";
                return false;
            }
            if (!appendThunkImports(
                    *original_thunk != 0 ? *original_thunk : *first_thunk,
                    *module, false, output, reason)) {
                return false;
            }
        }
        reason = "descriptor_out_of_bounds";
        return false;
    }

    bool appendDelayImports(const DataDirectory &directory,
                            std::vector<GameLogicPeImport> &output,
                            std::string &reason) const {
        if (directory.rva == 0 || directory.size == 0) return true;
        if (directory.size / 32 > maxImportDescriptors) {
            reason = "too_large";
            return false;
        }
        for (std::size_t index = 0; index * 32 + 32 <= directory.size; ++index) {
            const auto offset = directoryDescriptorOffset(directory, index, 32);
            if (!offset) {
                reason = "descriptor_out_of_bounds";
                return false;
            }
            const auto attributes = readValue<std::uint32_t>(*offset);
            const auto name_field = readValue<std::uint32_t>(*offset + 4);
            const auto iat_field = readValue<std::uint32_t>(*offset + 12);
            const auto int_field = readValue<std::uint32_t>(*offset + 16);
            if (!attributes || !name_field || !iat_field || !int_field) {
                reason = "truncated_delay_import_directory";
                return false;
            }
            if (*attributes == 0 && *name_field == 0 && *iat_field == 0 &&
                *int_field == 0) {
                return true;
            }
            const bool fields_are_rvas = (*attributes & 1U) != 0;
            const auto name_rva = delayFieldRva(*name_field, fields_are_rvas);
            const auto thunk_rva = delayFieldRva(
                *int_field != 0 ? *int_field : *iat_field, fields_are_rvas);
            if (!name_rva || !thunk_rva) {
                reason = "unsupported_delay_import_address";
                return false;
            }
            const auto name_offset = fileOffset(*name_rva);
            const auto module = name_offset
                                    ? readCString(*name_offset)
                                    : std::optional<std::string>{};
            if (!module) {
                reason = "invalid_delay_import_module_name";
                return false;
            }
            if (!appendThunkImports(*thunk_rva, *module, true, output,
                                    reason)) {
                return false;
            }
        }
        reason = "descriptor_out_of_bounds";
        return false;
    }
};

std::optional<std::string_view> legacyEntrance(std::string_view symbol) {
    constexpr std::array<std::string_view, 4> names{
        "__registerBehavior", "__registerEvent", "__emit",
        "__registerSystem"};
    for (const auto name : names) {
        if (symbol == name || symbol.find(std::string{"?"} + std::string{name} + "@") !=
                                  std::string_view::npos) {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> importedEpoch(std::string_view symbol) {
    constexpr std::string_view prefix = "pelican_game_logic_epoch_v";
    if (!symbol.starts_with(prefix)) return std::nullopt;
    std::uint32_t result = 0;
    const auto suffix = symbol.substr(prefix.size());
    if (suffix.empty()) return std::nullopt;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::string_view gameLogicPeImportKindName(
    GameLogicPeImportKind kind) noexcept {
    switch (kind) {
    case GameLogicPeImportKind::named:
        return "named";
    case GameLogicPeImportKind::ordinal:
        return "ordinal";
    case GameLogicPeImportKind::delay_named:
    case GameLogicPeImportKind::delay_ordinal:
        return "delay_load";
    }
    return "unknown";
}

GameLogicImportReadResult
readGameLogicImports(const std::filesystem::path &path) noexcept {
    GameLogicImportReadResult result;
    try {
        std::ifstream input{path, std::ios::binary | std::ios::ate};
        if (!input) {
            result.unreadable_reason = "open_failed";
            return result;
        }
        const auto end = input.tellg();
        if (end < 0) {
            result.unreadable_reason = "size_failed";
            return result;
        }
        const auto size = static_cast<std::uint64_t>(end);
        if (size > maxPeFileBytes ||
            size > std::numeric_limits<std::size_t>::max()) {
            result.unreadable_reason = "too_large";
            return result;
        }
        input.seekg(0);
        PeReader reader{input, size};
        DataDirectory imports;
        DataDirectory delay_imports;
        if (!reader.parseHeaders(imports, delay_imports,
                                 result.unreadable_reason)) {
            return result;
        }
        if (!reader.appendNormalImports(imports, result.imports,
                                        result.unreadable_reason) ||
            !reader.appendDelayImports(delay_imports, result.imports,
                                       result.unreadable_reason)) {
            result.imports.clear();
        }
    } catch (const std::exception &) {
        result.imports.clear();
        result.unreadable_reason = "reader_exception";
    } catch (...) {
        result.imports.clear();
        result.unreadable_reason = "reader_exception";
    }
    return result;
}

GameLogicImportDiagnosis diagnoseGameLogicImports(
    const std::filesystem::path &path, std::string_view host_module,
    std::uint32_t expected_epoch,
    std::uint32_t host_schema_entrance) noexcept {
    const auto read = readGameLogicImports(path);
    if (!read.readable()) {
        return {
            .error_code = std::string{gameLogicAbiUnreadableErrorCode},
            .error_fields = {{"reason", read.unreadable_reason}},
        };
    }

    const auto expected_module = asciiLower(host_module);
    std::vector<std::string> legacy;
    std::vector<std::uint32_t> epochs;
    std::uint32_t imported_schema_entrance = 2;
    for (const auto &entry : read.imports) {
        if (asciiLower(entry.module) != expected_module) continue;
        if (entry.kind == GameLogicPeImportKind::delay_named ||
            entry.kind == GameLogicPeImportKind::delay_ordinal) {
            return {
                .error_code =
                    std::string{gameLogicAbiUnsupportedImportErrorCode},
                .error_fields = {{"kind", "delay_load"}},
            };
        }
        if (entry.kind == GameLogicPeImportKind::ordinal) {
            return {
                .error_code =
                    std::string{gameLogicAbiUnsupportedImportErrorCode},
                .error_fields = {{"kind", "ordinal"}},
            };
        }
        if (const auto name = legacyEntrance(entry.symbol)) {
            legacy.emplace_back(*name);
        }
        if (const auto epoch = importedEpoch(entry.symbol)) {
            epochs.push_back(*epoch);
        }
        if (entry.symbol.find("pelican_register_behavior_v3") !=
                std::string_view::npos ||
            entry.symbol.find("pelican_register_event_v3") !=
                std::string_view::npos ||
            entry.symbol.find("pelican_register_component_v3") !=
                std::string_view::npos) {
            imported_schema_entrance = 3;
        }
    }

    if (imported_schema_entrance > host_schema_entrance) {
        return {
            .error_code = std::string{gameLogicAbiStaleErrorCode},
            .error_fields = {
                {"found", "v" + std::to_string(imported_schema_entrance)},
                {"host", "v" + std::to_string(host_schema_entrance)},
            },
        };
    }

    std::ranges::sort(legacy);
    legacy.erase(std::unique(legacy.begin(), legacy.end()), legacy.end());
    std::ranges::sort(epochs);
    epochs.erase(std::unique(epochs.begin(), epochs.end()), epochs.end());
    const bool expected_found =
        std::ranges::find(epochs, expected_epoch) != epochs.end();
    if (!expected_found || !legacy.empty()) {
        nlohmann::ordered_json fields{
            {"expected_epoch", expected_epoch},
            {"found", epochs.empty()
                          ? nlohmann::ordered_json(std::string{"absent"})
                          : nlohmann::ordered_json(
                                "v" + std::to_string(epochs.front()))},
        };
        if (!legacy.empty()) fields["legacy_symbols"] = legacy;
        return {
            .error_code = std::string{gameLogicAbiStaleErrorCode},
            .error_fields = std::move(fields),
        };
    }
    return {};
}

} // namespace Pelican::internal
