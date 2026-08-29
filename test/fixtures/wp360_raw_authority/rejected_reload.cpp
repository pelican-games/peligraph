#include "core/loader/authoringscenedocument.hpp"

void wp360_reload_must_not_acquire_raw(
    const Pelican::AuthoringSceneDocument &document) {
    (void)document.rawJson();
}
