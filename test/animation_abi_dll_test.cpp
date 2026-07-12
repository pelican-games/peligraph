#include "../src/core/userpublic/animation/abi_v1.hpp"
#include "fixtures/animation_abi/fixture_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <windows.h>

namespace Pelican::Animation {
namespace {

using LayoutFn = void (*)(AnimationAbiFixtureLayout *);
using WriteAdvanceFn = void (*)(void *, std::uint32_t);
using OldEngineFn = std::uint32_t (*)(void *, std::uint32_t);
using AcceptApiFn = std::uint32_t (*)(const void *, std::uint32_t);

struct FixtureDll {
    HMODULE module{};
    explicit FixtureDll(const char *path) : module(LoadLibraryA(path)) { REQUIRE(module != nullptr); }
    ~FixtureDll() { if (module) FreeLibrary(module); }
    template <class T> T symbol(const char *name) const {
        const auto address = GetProcAddress(module, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<T>(address);
    }
};

AnimationAbiFixtureLayout layout(const FixtureDll &dll) {
    AnimationAbiFixtureLayout value{};
    dll.symbol<LayoutFn>("pelican_animation_fixture_layout")(&value);
    return value;
}

} // namespace

TEST_CASE("old and new animation header DLLs preserve the frozen prefix", "[animation][a0][abi][dll]") {
    FixtureDll old_dll{PELICAN_ANIM_OLD_DLL};
    FixtureDll new_dll{PELICAN_ANIM_NEW_DLL};
    const auto old_layout = layout(old_dll);
    const auto new_layout = layout(new_dll);
    REQUIRE(old_layout.fixture_generation == 1);
    REQUIRE(new_layout.fixture_generation == 2);
    REQUIRE(old_layout.advance_size == new_layout.advance_size);
    REQUIRE(old_layout.advance_cursor_offset == new_layout.advance_cursor_offset);
    REQUIRE(old_layout.interval_crossings_offset == new_layout.interval_crossings_offset);
    REQUIRE(old_layout.api_context_offset == new_layout.api_context_offset);
    REQUIRE(old_layout.interval_size < new_layout.interval_size);
    REQUIRE(old_layout.api_size < new_layout.api_size);

    alignas(AdvanceDescV1) std::array<std::byte, sizeof(AdvanceDescV1) + 32> bytes{};
    bytes.fill(std::byte{0x5a});
    old_dll.symbol<WriteAdvanceFn>("pelican_animation_fixture_write_advance")(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    const auto *old_desc = reinterpret_cast<const AdvanceDescV1 *>(bytes.data());
    REQUIRE(old_desc->struct_size == old_layout.advance_size);
    REQUIRE(old_desc->cursor.generation == 1);
    REQUIRE(std::to_integer<unsigned>(bytes[sizeof(AdvanceDescV1)]) == 0x5a);
    bytes.fill(std::byte{0x5a});
    new_dll.symbol<WriteAdvanceFn>("pelican_animation_fixture_write_advance")(
        bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    const auto *new_desc = reinterpret_cast<const AdvanceDescV1 *>(bytes.data());
    REQUIRE(new_desc->cursor.generation == 2);
    REQUIRE(std::to_integer<unsigned>(bytes[sizeof(AdvanceDescV1)]) == 0x5a);
}

TEST_CASE("old-client new-engine and new-client old-engine negotiate additively", "[animation][a0][abi][dll]") {
    FixtureDll old_dll{PELICAN_ANIM_OLD_DLL};
    FixtureDll new_dll{PELICAN_ANIM_NEW_DLL};
    const auto old_layout = layout(old_dll);

    alignas(ApiV1) std::array<std::byte, sizeof(ApiV1) + 32> storage{};
    storage.fill(std::byte{0x5a});
    auto *api = reinterpret_cast<ApiV1 *>(storage.data());
    api->struct_size = old_layout.api_size;
    api->version = 1;
    api->reserved0 = 0;
    api->reserved1 = 0;
    REQUIRE(getApiV1(1, api) == Status::ok);
    REQUIRE(old_dll.symbol<AcceptApiFn>("pelican_animation_fixture_accept_api")(api, old_layout.api_size) == 1);
    REQUIRE(std::to_integer<unsigned>(storage[old_layout.api_size]) == 0x5a);

    storage.fill(std::byte{0x5a});
    api = reinterpret_cast<ApiV1 *>(storage.data());
    api->struct_size = sizeof(ApiV1);
    api->version = 1;
    const auto written = old_dll.symbol<OldEngineFn>("pelican_animation_fixture_old_engine")(api, sizeof(ApiV1));
    REQUIRE(written == old_layout.api_size);
    REQUIRE(new_dll.symbol<AcceptApiFn>("pelican_animation_fixture_accept_api")(api, sizeof(ApiV1)) == 1);
    REQUIRE(std::to_integer<unsigned>(storage[old_layout.api_size]) == 0x5a);
}

} // namespace Pelican::Animation
