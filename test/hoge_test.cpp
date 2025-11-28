#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("example test" "[hoge]") {
    REQUIRE(1 == 1);
}

}
