#include "gamelogicimports.hpp"

#include <iostream>
#include <string>

int main() {
    const auto diagnosis = Pelican::internal::diagnoseGameLogicImports(
        WP381_MUTATION_FIXTURE, WP381_MUTATION_HOST, 2);
    if (!diagnosis.accepted()) {
        std::cerr << "mutant was not observable: " << diagnosis.error_code
                  << diagnosis.error_fields.dump() << '\n';
        return 1;
    }
    return 0;
}
