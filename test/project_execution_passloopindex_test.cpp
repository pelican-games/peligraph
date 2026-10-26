#include "../src/project/passloopindex.hpp"

bool projectPassLoopIndexDefaults() {
    return Pelican::PassLoopIndex{} == Pelican::PassLoopIndex{0, 1};
}
