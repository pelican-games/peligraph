#pragma once

#include <details/ecs/componentdeclare.hpp>

#include <cstdint>
#include <iostream>
#include <span>
#include <tuple>
#include <utility>

namespace Pelican {

// TODO
using GameObjectId = uint64_t;
using ComponentId = uint64_t;

class GameObjects {
  private:
    template <ComponentId... id> struct ComponentIdHolder {
        template <class T> using Append = ComponentIdHolder<id..., ComponentIdByType<T>::value>;
        constexpr static std::span<const ComponentId> ids() {
            static constexpr ComponentId _ids[] = {id...};
            return std::span<const ComponentId>{_ids, sizeof...(id)};
        }
        static constexpr size_t len = sizeof...(id);
    };

    template <size_t index, size_t... indices> struct IndicesDecoder {
        static constexpr size_t first = index;
        using Remain = IndicesDecoder<indices...>;
    };
    template <size_t index> struct IndicesDecoder<index> {
        static constexpr size_t first = index;
        using Remain = void;
    };
    template <size_t... index> struct IndexHolder {
        template <size_t new_index> using Append = IndexHolder<index..., new_index>;
        static constexpr size_t first = IndicesDecoder<index...>::first;
        using RemainDecoder = IndicesDecoder<index...>::Remain;
    };
    template <class Indices, size_t i> struct IndicesAt {
        static constexpr size_t value = IndicesAt<Indices::RemainDecoder, i - 1>::value;
    };
    template <class Indices> struct IndicesAt<Indices, 0> {
        static constexpr size_t value = Indices::first;
    };

    static GameObjectId alloc(const ComponentId *ids, void **ptrs, uint32_t components_count);
    static void commit(const ComponentId *ids, void *const *ptrs, uint32_t components_count);

    template <class Indices, class Tuple, size_t... Seq>
    static void copy(void **ptrs, Tuple t, Indices indices, std::index_sequence<Seq...>) {
        (([&]() {
             using TComponent = std::remove_cvref_t<std::tuple_element_t<Seq, Tuple>>;
             constexpr size_t i = IndicesAt<Indices, Seq>::value;
             *static_cast<TComponent *>(ptrs[i]) = std::get<Seq>(t);
         })(),
         ...);
    }

  public:
    template <class ComponentIds, class DataComponentIndices, class ComponentDataTuple> struct AddGameObjectContext {
        ComponentDataTuple data;
        template <class T> auto addComponent() {
            using NewComponentIds = typename ComponentIds::template Append<T>;
            return AddGameObjectContext<NewComponentIds, DataComponentIndices, ComponentDataTuple>{data};
        }
        template <class T> auto addComponent(const T &component) {
            using NewComponentIds = typename ComponentIds::template Append<T>;
            using NewComponentIndices = typename DataComponentIndices::template Append<ComponentIds::len>;
            auto data2 = std::tuple_cat(data, std::tuple<const T &>(component));
            return AddGameObjectContext<NewComponentIds, NewComponentIndices, decltype(data2)>{data2};
        }
        void finish() {
            auto ids = ComponentIds::ids();
            void *ptrs[ComponentIds::len];
            GameObjects::alloc(ids.data(), ptrs, std::size(ptrs));
            GameObjects::copy(ptrs, data, DataComponentIndices{},
                              std::make_index_sequence<std::tuple_size<ComponentDataTuple>::value>());
            GameObjects::commit(ids.data(), ptrs, std::size(ptrs));
        };
    };

    template <class ComponentIds> struct AddGameObjectContextWithoutData {
        template <class T> auto addComponent() {
            using NewComponentIds = ComponentIds::template Append<T>;
            return AddGameObjectContextWithoutData<NewComponentIds>{};
        }
        template <class T> auto addComponent(const T &component) {
            using NewComponentIds = ComponentIds::template Append<T>;
            auto data = std::tuple<const T &>(component);
            return AddGameObjectContext<NewComponentIds, IndexHolder<ComponentIds::len>, decltype(data)>{data};
        }
        void finish() {
            auto ids = ComponentIds::ids();
            void *ptrs[ComponentIds::len];
            GameObjects::alloc(ids.data(), ptrs, std::size(ptrs));
            GameObjects::commit(ids.data(), ptrs, std::size(ptrs));
        };
    };

    struct AddGameObjectContextEmpty {
        template <class T> auto addComponent() {
            return AddGameObjectContextWithoutData<ComponentIdHolder<ComponentIdByType<T>::value>>{};
        }
        template <class T> auto addComponent(const T &component) {
            auto data = std::tuple<const T &>(component);
            return AddGameObjectContext<ComponentIdHolder<ComponentIdByType<T>::value>, IndexHolder<0>, decltype(data)>{
                data};
        }
    };

    static auto add() { return AddGameObjectContextEmpty{}; }
    static void remove(GameObjectId id);
};

} // namespace Pelican
