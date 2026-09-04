#include <details/schema/structfieldjson.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

namespace {

constinit bool count_allocations = false;
constinit std::size_t allocation_count = 0;
constinit std::size_t descriptor_allocation_count = 1;
constinit std::size_t descriptor_node_count = 0;

struct Row {
    std::string name;
    bool enabled = false;
    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_allocation_row"),
        Pelican::required(Pelican::field<&Row::name>("name")),
        Pelican::defaulted(Pelican::field<&Row::enabled>("enabled"), false));
};

struct Root {
    Pelican::List<Row> rows;
    static constexpr auto schema = Pelican::structFields(
        Pelican::componentPolicy("wp382a_allocation_root"),
        Pelican::defaulted(Pelican::field<&Root::rows>("rows")));
};

struct StaticDescriptorProbe {
    StaticDescriptorProbe() {
        allocation_count = 0;
        count_allocations = true;
        const auto descriptor = Pelican::internal::materializeSchemaNode(
            Root::schema);
        count_allocations = false;
        descriptor_allocation_count = allocation_count;
        descriptor_node_count = descriptor.node_count;
    }
};

StaticDescriptorProbe static_descriptor_probe;

} // namespace

void *operator new(std::size_t size) {
    if (count_allocations) ++allocation_count;
    if (void *memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    return descriptor_allocation_count == 0 && descriptor_node_count == 5
               ? 0
               : 1;
}
