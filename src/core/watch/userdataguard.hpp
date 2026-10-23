#pragma once
#include "problemlist.hpp"
#include "../userpublic/userdataerror.hpp"
#include <details/ecs/entity.hpp>
#include <set>
#include <utility>

namespace Pelican {
struct UserDataScope {
    std::string key;
    std::string version;
    nlohmann::json fields = nlohmann::json::object();
    std::string code = "runtime.user_data@1";
    // Entity scopes are evaluated by their owner-thread prepare/update path.
    // Resolve names only on failure; successful sprite frames need no scene scan.
    std::function<nlohmann::json()> entity_fields;
};
// ECS worker scopes carry system metadata captured before scheduling.
UserDataScope userDataScope(std::string system, std::string component = {},
                            EntityId entity = invalidGameObjectId,
                            std::string version = {});
std::string userDataFileVersion(std::string_view reference);
bool beginUserData(const UserDataScope &scope);
void failUserData(const UserDataScope &scope, const char *message);
void retainUserDataScopes(std::string_view prefix, const std::set<std::string> &live);
void refreshBehaviorUserDataFailures();
void requestUserCodeStop();
bool userCodeStopRequested();
void finishUserCodeStop();

template<class F> bool guardUserData(const UserDataScope &scope, F &&evaluate) {
    if (!beginUserData(scope)) return false;
    try {
        std::forward<F>(evaluate)();
        return true;
    } catch (const UserDataError &error) {
        failUserData(scope, error.what());
        return false;
    }
}
// The callback itself belongs to the game DLL; std::exception from that
// boundary is authored code. Do not apply this conversion to engine work.
template<class F> bool guardUserCode(const UserDataScope &scope, F &&callback) {
    return guardUserData(scope, [&] {
        try { std::forward<F>(callback)(); }
        catch (const std::exception &error) {
            requestUserCodeStop();
            throw UserDataError(scope.fields.value("behavior", scope.key) + ": " + error.what());
        }
    });
}
}
