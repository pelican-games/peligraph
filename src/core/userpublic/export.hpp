#pragma once

#if defined(_WIN32)
#if defined(PELICAN_ENGINE_BUILD)
#define PELICAN_API __declspec(dllexport)
#elif defined(PELICAN_GAME_DLL)
#define PELICAN_API __declspec(dllimport)
#else
#define PELICAN_API
#endif
#else
#define PELICAN_API __attribute__((visibility("default")))
#endif
