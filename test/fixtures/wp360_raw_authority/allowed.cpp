#include "core/loader/authoringsceneauthority.hpp"

void wp360_authoring_authority_allowed(
    const Pelican::AuthoringSceneDocument &document) {
    const Pelican::AuthoringSceneRawView raw =
        Pelican::AuthoringSceneAuthority::rawView(document);
    (void)raw.scenesJson();
}
