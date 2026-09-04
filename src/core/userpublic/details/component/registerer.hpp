#pragma once

#include <details/ecs/component.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/reload/registrationowner.hpp>
#include <details/schema/structfieldjson.hpp>
#include <nlohmann/json.hpp>
#include <serialize/jsonarchive.hpp>
#include <serialize/serialize.hpp>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Pelican {

namespace internal {

class EngineComponentRegisterer;

struct FrozenComponentLoaderInfoV2Layout {
    std::string name;
    size_t alignment;
    void (*construct)(void *ptr);
    void (*destroy)(void *ptr) noexcept;
    void (*relocate)(void *dst, void *src) noexcept;
    void (*init)(void *ptr);
    void (*deinit)(void *ptr) noexcept;
    void (*json_loader)(void *component, JsonArchiveLoader &ar);
};

// V3 deliberately does not reuse or relocate V2's nested ComponentLoaderInfo:
// its qualified type name is part of the frozen decorated entrance symbol.
struct ComponentRegistrationV3 {
    ComponentId id = 0;
    size_t size = 0;
    std::string name;
    size_t alignment = 0;
    void (*construct)(void *ptr) = nullptr;
    void (*destroy)(void *ptr) noexcept = nullptr;
    void (*relocate)(void *dst, void *src) noexcept = nullptr;
    void (*init)(void *ptr) = nullptr;
    void (*deinit)(void *ptr) noexcept = nullptr;
    void (*json_loader)(void *component, JsonArchiveLoader &ar) = nullptr;
    const SchemaNode &schema;
};

enum class ComponentRegistrationPolicyErrorCode : std::uint8_t {
    builtin_name_unprefixed,
    reserved_component_namespace,
};

PELICAN_API std::string_view componentRegistrationPolicyErrorCodeName(
    ComponentRegistrationPolicyErrorCode code) noexcept;

class PELICAN_API ComponentRegistrationPolicyError final
    : public std::runtime_error {
    ComponentRegistrationPolicyErrorCode code_;
    nlohmann::ordered_json payload_;

  public:
    ComponentRegistrationPolicyError(
        ComponentRegistrationPolicyErrorCode code, std::string name,
        RegistrationOwner owner);

    ComponentRegistrationPolicyErrorCode code() const noexcept {
        return code_;
    }
    const nlohmann::ordered_json &payload() const noexcept { return payload_; }
};

class UserComponentRegistererTemplatePublic {
    friend class EngineComponentRegisterer;

    struct ComponentLoaderInfo {
        std::string name;
        size_t alignment;
        void (*construct)(void *ptr);
        void (*destroy)(void *ptr) noexcept;
        void (*relocate)(void *dst, void *src) noexcept;
        void (*init)(void *ptr);
        void (*deinit)(void *ptr) noexcept;
        void (*json_loader)(void *component, JsonArchiveLoader &ar);
    };
    static_assert(sizeof(ComponentLoaderInfo) ==
                      sizeof(FrozenComponentLoaderInfoV2Layout),
                  "ComponentLoaderInfo is the frozen v2 ABI record");

    PELICAN_API RegistrationToken
    __registerComponent(ComponentId id, size_t sz, ComponentLoaderInfo loader);
    PELICAN_API RegistrationToken
    pelican_register_component_v3(ComponentRegistrationV3 registration);
    RegistrationToken __registerEngineComponent(ComponentId id, size_t sz,
                                                ComponentLoaderInfo loader);
    RegistrationToken __registerComponentForOwner(ComponentId id, size_t sz,
                                                  ComponentLoaderInfo loader,
                                                  RegistrationOwner owner,
                                                  std::optional<SchemaNode> schema =
                                                      std::nullopt);

    template <class Component, bool RecursiveSchema = false>
    static ComponentLoaderInfo componentLoader(std::string name) {
        static_assert(std::is_default_constructible_v<Component>,
                      "ECS components must be default constructible");
        static_assert(std::is_nothrow_move_constructible_v<Component>,
                      "ECS components must be noexcept move constructible");
        static_assert(std::is_nothrow_destructible_v<Component>,
                      "ECS components must be noexcept destructible");
        if constexpr (requires(Component &component) { component.deinit(); }) {
            static_assert(noexcept(std::declval<Component &>().deinit()),
                          "ECS component deinit() must be noexcept");
        }

        return ComponentLoaderInfo{
                .name = name,
                .alignment = alignof(Component),
                .construct = [](void *c) { std::construct_at(static_cast<Component *>(c)); },
                .destroy = [](void *c) noexcept { std::destroy_at(static_cast<Component *>(c)); },
                .relocate = [](void *dst, void *src) noexcept {
                    if constexpr (std::is_trivially_copyable_v<Component>) {
                        std::memcpy(dst, src, sizeof(Component));
                        if constexpr (!std::is_trivially_destructible_v<Component>) {
                            std::destroy_at(static_cast<Component *>(src));
                        }
                    } else {
                        std::construct_at(static_cast<Component *>(dst),
                                          std::move(*static_cast<Component *>(src)));
                        std::destroy_at(static_cast<Component *>(src));
                    }
                },
                .init = []() -> void (*)(void *) {
                    if constexpr (requires(Component &component) { component.init(); }) {
                        return [](void *c) { static_cast<Component *>(c)->init(); };
                    }
                    return nullptr;
                }(),
                .deinit = []() -> void (*)(void *) noexcept {
                    if constexpr (requires(Component &component) { component.deinit(); }) {
                        return [](void *c) noexcept { static_cast<Component *>(c)->deinit(); };
                    }
                    return nullptr;
                }(),
                .json_loader = []() -> void (*)(void *, JsonArchiveLoader &) {
                    if constexpr (RecursiveSchema) {
                        return [](void *c, JsonArchiveLoader &ar) {
                            auto authored = *static_cast<const nlohmann::json *>(
                                ar.ptr);
                            authored.erase("name");
                            decodeStructObject(authored,
                                               *static_cast<Component *>(c),
                                               Component::schema, "component");
                        };
                    } else if constexpr (ISerializable<Component, JsonArchiveLoader>) {
                        return [](void *c, JsonArchiveLoader &ar) { static_cast<Component *>(c)->ref(ar); };
                    }
                    return nullptr;
                }(),
            };
    }

  public:
    template <class Component>
    RegistrationToken registerComponent(std::string name) {
        constexpr bool has_recursive_schema = [] {
            if constexpr (requires { Component::schema; })
                return schemaRequiresV3(Component::schema);
            return false;
        }();
        if constexpr (has_recursive_schema) {
            static const auto schema = materializeSchemaNode(Component::schema);
            auto loader = componentLoader<Component, true>(std::move(name));
            return pelican_register_component_v3(ComponentRegistrationV3{
                ComponentIdByType<Component>::value, sizeof(Component),
                std::move(loader.name), loader.alignment, loader.construct,
                loader.destroy, loader.relocate, loader.init, loader.deinit,
                loader.json_loader, schema});
        } else {
            return __registerComponent(
                ComponentIdByType<Component>::value, sizeof(Component),
                componentLoader<Component>(std::move(name)));
        }
    }
};

PELICAN_API UserComponentRegistererTemplatePublic &getComponentRegisterer();

} // namespace internal

}; // namespace Pelican
