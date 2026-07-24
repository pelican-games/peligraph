#include "../src/core/vkcore/frametarget.hpp"

#include <catch2/catch_test_macros.hpp>
#include <memory>

using namespace Pelican;

TEST_CASE(
    "GPU submission slots retain a generation until the matching completion",
    "[wp196][render-pipeline][submission-lifetime]") {
    GpuSubmissionLeaseSlots<2> slots;
    auto generation = std::make_shared<const int>(17);
    std::weak_ptr<const int> observed = generation;

    slots.submitted(0, generation);
    generation.reset();

    REQUIRE_FALSE(observed.expired());
    REQUIRE(slots.outstandingForTesting() == 1);

    slots.complete(1);
    REQUIRE_FALSE(observed.expired());
    REQUIRE(slots.outstandingForTesting() == 1);

    slots.complete(0);
    REQUIRE(observed.expired());
    REQUIRE(slots.outstandingForTesting() == 0);
}

TEST_CASE(
    "GPU submission slots retire independent in-flight generations",
    "[wp196][render-pipeline][submission-lifetime]") {
    GpuSubmissionLeaseSlots<2> slots;
    auto first = std::make_shared<const int>(1);
    auto second = std::make_shared<const int>(2);
    std::weak_ptr<const int> observed_first = first;
    std::weak_ptr<const int> observed_second = second;

    slots.submitted(0, first);
    slots.submitted(1, second);
    first.reset();
    second.reset();

    slots.complete(0);
    REQUIRE(observed_first.expired());
    REQUIRE_FALSE(observed_second.expired());

    slots.completeAll();
    REQUIRE(observed_second.expired());
}
