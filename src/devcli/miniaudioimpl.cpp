// AB-1: miniaudio's implementation for pelican_cli, which decodes a sound
// with the engine's own decoders (core/audio/sounddecode.cpp) but does not
// link the engine. The same setup as audio.cpp: stb_vorbis's declarations
// before miniaudio so its Vorbis decoding backend is compiled, its
// implementation after.
#define STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4245 4456 4457 4701 4996)
#endif
#undef STB_VORBIS_HEADER_ONLY
#include <extras/stb_vorbis.c>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
