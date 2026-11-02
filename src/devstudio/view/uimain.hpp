#pragma once

#include <string>

namespace PelicanStudio {

// What the process was asked to open before the user touches anything. Both
// fields are optional; an empty one means "leave it as the window decides".
struct LaunchOptions {
    // A project directory to open as if through File > Open Project.
    std::string project;
    // A render tool to bring to the front (UI-2: its dock): one of the ids
    // isRenderTool() accepts.
    std::string tab;
    // The local socket name the test-automation channel listens on. Empty --
    // which is every shipping launch -- means no channel is opened at all: the
    // server object is never constructed, so the socket does not exist.
    std::string automation;
};

// Whether `name` is a render tool id the window can show. The caller
// checks before building a window so an unknown name fails on the console
// rather than silently opening the wrong panel.
bool isRenderTool(const std::string &name);
std::string renderToolList();

int uimain(int argc, char **argv, LaunchOptions options = {});

}
