#pragma once

#include <unordered_map>

namespace Pelican {

template <class T, class Base> struct BasicHandle {
    Base value;
    operator Base() const { return value; }
    bool operator==(BasicHandle o) const { return value == o.value; }
    bool operator!=(BasicHandle o) const { return value != o.value; }
    bool operator<(BasicHandle o) const { return value < o.value; }
    struct Hash {
        size_t operator()(T key) const { return std::hash<Base>{}(key.value); }
    };
    using BaseType = Base;
};

struct Hoge : public BasicHandle<Hoge, uint32_t> {};
#define PELICAN_DEFINE_HANDLE(name, base)                                                                              \
    struct name : public BasicHandle<name, base> {};

} // namespace Pelican
