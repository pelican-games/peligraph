#pragma once

#include <cstdint>

namespace Pelican {

// TODO
using GameObjectId = uint64_t;
using ComponentId = uint64_t;

class GameObjects {
  private:
    static GameObjectId add(ComponentId *ids, void **ptrs, uint32_t components_count);

  public:
    template <class... TComponents> static GameObjectId add(const TComponents &...component) {
        // TODO
        // ComponentId ids[] = {ComponentIdByType<TComponents>()::value...};
        // void *ptrs[sizeof...(TComponents)];
        // GameObjectId oid = add(ids, ptrs, sizeof...(TComponents));
        // return oid;
        return 0;
    }
    static void remove(GameObjectId id);
};

} // namespace Pelican
