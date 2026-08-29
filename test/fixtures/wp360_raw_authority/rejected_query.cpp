#include "core/loader/authoringscenedocument.hpp"

void wp360_query_must_not_acquire_raw(
    const Pelican::AuthoringSceneDocument &document) {
    (void)document.rawJson();
}
