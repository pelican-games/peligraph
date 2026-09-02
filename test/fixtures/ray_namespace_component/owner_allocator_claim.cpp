#include <details/reload/registrationowner.hpp>

extern "C" __declspec(dllexport) void wp379a_allocate_owner() {
    (void)Pelican::internal::allocateRegistrationOwner();
}
