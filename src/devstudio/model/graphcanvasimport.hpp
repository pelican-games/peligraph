#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// SG-2: the files the Layers dock brings into a project -- pictures for
// + Image and Add mask image, and a PSD's conversion (pelican-import-tools'
// psd-canvas output) for Import PSD output. Qt-free; nothing here edits the
// document, the dock sends the edit once the files are in place.
namespace PelicanStudio {

// Pictures copied into a project's assets/canvas, each under its own name,
// `_2`, `_3`... when that name is taken (the audio import's rule, UX-8
// decision 9). `references` are `project://assets/canvas/<name>`, `copies`
// the files written, in the same order; `error` says why one was not copied
// (not a .png / .jpg / .jpeg, unreadable). Nothing is overwritten.
struct GraphCanvasImageImport {
    std::vector<std::string> references;
    std::vector<std::filesystem::path> copies;
    std::string error;
};
[[nodiscard]] GraphCanvasImageImport importCanvasImages(const std::filesystem::path &project_root,
                                                        const std::vector<std::filesystem::path> &files);
// Whether a path names a picture + Image takes (by its extension).
[[nodiscard]] bool isCanvasImageFile(const std::filesystem::path &file);

// A psd-canvas output directory, read: its canvas.graph.json's one
// definition and, from canvas.report.json when there is one, the layer count
// and the diagnostics (one line each). `directory` is the name the copy in
// the project takes (`imports/<directory>`): the one its assets already
// name (`--asset-prefix project://imports/<directory>`), else the
// definition's. `error` is set, and nothing else is, when it is not one.
struct GraphCanvasPsdOutput {
    std::filesystem::path source;
    std::string definition_name;
    nlohmann::json definition;
    std::string directory;
    std::size_t layers = 0;
    std::vector<std::string> diagnostics;
    std::string error;
};
[[nodiscard]] GraphCanvasPsdOutput readCanvasPsdOutput(const std::filesystem::path &graph_json);
// The output's files -- canvas.graph.json, canvas.report.json, layers/ and
// masks/ -- copied to `<project>/imports/<directory>`; one already there is
// replaced only when `replace` says so. Empty on success, else why not.
[[nodiscard]] std::string copyCanvasPsdOutput(const std::filesystem::path &project_root,
                                              const GraphCanvasPsdOutput &output, bool replace);

} // namespace PelicanStudio
