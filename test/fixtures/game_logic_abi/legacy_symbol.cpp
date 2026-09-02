#if defined(WP381_LEGACY_BEHAVIOR)
#pragma comment(linker, "/include:__imp_?__registerBehavior@UserBehaviorRegistererTemplatePublic@internal@Pelican@@AEAA?AVRegistrationToken@23@UBehaviorRegistration@23@@Z")
#elif defined(WP381_LEGACY_EVENT)
#pragma comment(linker, "/include:__imp_?__registerEvent@UserEventRegistererTemplatePublic@internal@Pelican@@AEAA?AVRegistrationToken@23@UEventTypeRegistration@23@@Z")
#elif defined(WP381_LEGACY_EMIT)
#pragma comment(linker, "/include:__imp_?__emit@UserEventRegistererTemplatePublic@internal@Pelican@@AEAAXUQueuedEvent@23@@Z")
#elif defined(WP381_LEGACY_SYSTEM)
#pragma comment(linker, "/include:__imp_?__registerSystem@UserGameSystemRegistererTemplatePublic@internal@Pelican@@AEAA?AVRegistrationToken@23@UGameSystemRegistration@23@@Z")
#else
#error A single WP381 legacy entrance must be selected
#endif

extern "C" __declspec(dllexport) int wp381_legacy_symbol_fixture() {
    return 381;
}
