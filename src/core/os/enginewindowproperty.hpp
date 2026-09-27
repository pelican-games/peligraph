#pragma once

// EW-1: the mark on the engine's render window. The engine sets this Win32
// window property (value 1) on its GLFW window right after creating it, on
// every windowed run, and removes it before the window is destroyed. A host
// that embeds the engine (Studio) finds the window as "the top-level window
// of that process, with no owner, carrying this property", whether or not it
// is visible yet: an --embedded engine creates its window hidden.
//
// Header-only and free of engine types so that a host that must not link the
// engine can include it.

namespace Pelican {

inline constexpr wchar_t engineRenderWindowProperty[] = L"PelicanEngineRenderWindow";

} // namespace Pelican
