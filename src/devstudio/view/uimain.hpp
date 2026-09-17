#pragma once

#include <string>

namespace PelicanStudio {

// What the process was asked to open before the user touches anything. Both
// fields are optional; an empty one means "leave it as the window decides".
struct LaunchOptions {
    // A project directory to open as if through File > Open Project.
    std::string project;
    // A render-tools tab to bring to the front: one of the ids
    // isRenderToolsTab() accepts.
    std::string tab;
};

// Whether `name` is a render-tools tab id the window can show. The caller
// checks before building a window so an unknown name fails on the console
// rather than silently opening the wrong tab.
bool isRenderToolsTab(const std::string &name);
std::string renderToolsTabList();

int uimain(int argc, char **argv, LaunchOptions options = {});

}
