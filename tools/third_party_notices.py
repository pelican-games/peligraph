#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Generate THIRD_PARTY_NOTICES.md: the license texts of the third-party code
pelican2 builds in, ships next to, or derives code from.

One table (THIRD_PARTY below) says, per entry, how the code is used and where
its license text is. The text is copied verbatim from that file -- the LICENSE
of the dependency CMake fetched (<build>/_deps/<name>-src), a vendored file in
this repository, or the Vulkan SDK. The version of a fetched dependency is
read from its FetchContent_Declare. A text that is not on this machine is
listed as not acquired instead of being written from memory.

    uv run tools/third_party_notices.py                       # write the file
    uv run tools/third_party_notices.py --check               # exit 1 if stale
    uv run tools/third_party_notices.py --check-declarations  # exit 1 if the
        FetchContent_Declare names in the CMake files and THIRD_PARTY differ

Options: --repo-root DIR, --deps-dir DIR (default <repo>/build/_deps),
--vulkan-sdk DIR (default $VULKAN_SDK), --output FILE.

When a dependency is added to CMake, add its row to THIRD_PARTY (the check
fails until then) and regenerate. Standard library only.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# A license text source: (root, relative path[, first line, last line]).
#   root "deps": the dependency's FetchContent source dir under --deps-dir
#   root "repo": this repository
#   root "sdk":  the Vulkan SDK (--vulkan-sdk)
# Line numbers are 1-based and inclusive (a license kept in a file's header).


@dataclass
class Entry:
    name: str
    use: str
    license: str
    texts: list[tuple] = field(default_factory=list)
    fetch: str | None = None          # FetchContent name, if CMake fetches it
    upstream: str | None = None       # for entries CMake does not fetch
    version: str | None = None        # for entries CMake does not fetch
    missing: list[str] = field(default_factory=list)  # texts not acquired
    notes: list[str] = field(default_factory=list)


THIRD_PARTY: list[Entry] = [
    # --- Fetched by CMake (FetchContent) -----------------------------------
    Entry("quill (with its bundled {fmt})", fetch="quill",
          use="Linked into pelican_core: in every shipped executable.",
          license="MIT (quill); MIT with an optional object-code exception ({fmt})",
          texts=[("deps", "LICENSE"),
                 ("deps", "include/quill/bundled/fmt/format.h", 1, 31)]),
    Entry("battery-embed", fetch="battery-embed",
          use="Build tool: turns the engine's and Studio's resources into C++ "
              "sources compiled into the executables.",
          license="Apache-2.0",
          texts=[("deps", "LICENSE")]),
    Entry("GLM", fetch="glm",
          use="Header-only maths library compiled into the executables.",
          license="Happy Bunny License or MIT",
          texts=[("deps", "copying.txt")]),
    Entry("miniaudio", fetch="miniaudio",
          use="Compiled into pelican_core when audio is enabled.",
          license="Public Domain (Unlicense) or MIT No Attribution",
          texts=[("deps", "LICENSE")]),
    Entry("stb_vorbis (bundled with miniaudio)",
          upstream="https://github.com/mackron/miniaudio/blob/0.11.25/extras/stb_vorbis.c",
          version="1.22 (miniaudio 0.11.25)",
          use="Ogg Vorbis decoder compiled into the audio backend.",
          license="MIT OR Unlicense",
          texts=[("deps:miniaudio", "extras/stb_vorbis.c", 5548, 5586)]),
    Entry("JSON for Modern C++ (nlohmann/json)", fetch="nlohmann",
          use="Header-only, compiled into the executables.",
          license="MIT",
          texts=[("deps", "LICENSE.MIT")]),
    Entry("argparse", fetch="argparse",
          use="Header-only, compiled into Studio and the CLI.",
          license="MIT",
          texts=[("deps", "LICENSE")]),
    Entry("PicoSHA2", fetch="picosha2",
          use="Header-only SHA-256, compiled into the executables.",
          license="MIT",
          texts=[("deps", "LICENSE")]),
    Entry("SPIRV-Headers", fetch="spirv_headers",
          use="Headers used by SPIRV-Reflect and SPIRV-Tools.",
          license="MIT (Khronos)",
          texts=[("deps", "LICENSE")]),
    Entry("SPIRV-Tools", fetch="spirv_tools",
          use="Linked only with PELICAN_WITH_SPIRV_LINK (off by default) and "
              "in pelican-spv-link. The Vulkan SDK's shaderc_combined, linked "
              "into Release builds, also contains SPIRV-Tools (see the Vulkan "
              "SDK entry).",
          license="Apache-2.0",
          texts=[("deps", "LICENSE")]),
    Entry("SPIRV-Reflect", fetch="spirv_reflect",
          use="Statically linked into pelican_core.",
          license="Apache-2.0",
          texts=[("deps", "LICENSE")]),
    Entry("stb", fetch="stb",
          use="stb_image, stb_image_write and stb_truetype, compiled into "
              "the executables.",
          license="MIT or Public Domain (Unlicense)",
          texts=[("deps", "LICENSE")]),
    Entry("TinyEXR", fetch="tinyexr",
          use="Compiled into pelican_core (EXR images). Built with "
              "TINYEXR_USE_MINIZ=0 and TINYEXR_USE_STB_ZLIB=1, so its bundled "
              "miniz, zstd and ZFP are not compiled.",
          license="BSD-3-Clause (NOTICE lists the third-party parts)",
          texts=[("deps", "LICENSE"), ("deps", "NOTICE")]),
    Entry("OpenXR-SDK (loader)", fetch="openxr_sdk",
          use="The OpenXR loader, statically linked when OpenXR is enabled. "
              "The loader embeds JsonCpp.",
          license="Apache-2.0 OR MIT (loader); JsonCpp: Public Domain or MIT",
          texts=[("deps", "LICENSE"), ("deps", "COPYING.adoc"),
                 ("deps", "LICENSES/MIT.txt"),
                 ("deps", "LICENSES/LicenseRef-jsoncpp-public-domain.txt")]),
    Entry("Jolt Physics", fetch="joltphysics",
          use="Compiled in only with the Jolt physics option (off by default).",
          license="not checked (the source is not fetched in this build)",
          missing=["Jolt Physics LICENSE (the build that generated this file "
                   "did not fetch it)"]),
    Entry("VulkanMemoryAllocator-Hpp (with VulkanMemoryAllocator)", fetch="vma-hpp",
          use="Header-only, compiled into pelican_core.",
          license="CC0-1.0 (the C++ bindings); MIT (VulkanMemoryAllocator, AMD)",
          texts=[("deps", "LICENSE"),
                 ("deps", "VulkanMemoryAllocator/LICENSE.txt")]),
    Entry("Dear ImGui", fetch="imgui_vendor",
          use="Compiled into pelican_core when the ImGui option is on.",
          license="MIT",
          texts=[("deps", "LICENSE.txt")]),
    Entry("TinyGLTF", fetch="tinygltf",
          use="Compiled into pelican_core (glTF / VRM loading).",
          license="MIT",
          texts=[("deps", "LICENSE")]),
    Entry("GLFW", fetch="glfw",
          use="Linked into pelican_core (windows and input).",
          license="Zlib",
          texts=[("deps", "LICENSE.md")]),
    Entry("Catch2", fetch="Catch2",
          use="Tests only; not in any shipped file.",
          license="BSL-1.0",
          texts=[("deps", "LICENSE.txt")]),
    # --- Vendored in this repository ---------------------------------------
    Entry("RenderDoc in-application API header",
          upstream="https://github.com/baldurk/renderdoc (src/third_party/renderdoc/README.md "
                   "records the tag, commit and SHA-256)",
          version="v1.45",
          use="One header, src/third_party/renderdoc/renderdoc_app.h. No "
              "RenderDoc binary is linked or distributed.",
          license="MIT",
          texts=[("repo", "src/third_party/renderdoc/renderdoc_app.h", 1, 23)]),
    # --- Code derived from third-party implementations ---------------------
    Entry("PCG random number generation (pcg-c-basic)",
          upstream="https://www.pcg-random.org (Melissa E. O'Neill)",
          version="-",
          use="src/core/userpublic/deterministicrng.cpp (the public SDK's "
              "DeterministicRng) follows pcg_basic.c: its constants, output "
              "step, seeding and bounded draw. The hash in "
              "projects/pathtrace_demo/shaders/pt_rng.glsl is PCG's RXS-M-XS "
              "output over a 32-bit LCG step.",
          license="Apache-2.0",
          texts=[("repo", "third_party/licenses/pcg-c-basic-LICENSE.txt")],
          notes=["The license text below is pcg-c-basic's own LICENSE.txt "
                 "(third_party/licenses/README.md says where and when it was "
                 "copied). pcg-c-basic has no NOTICE file."]),
    Entry("boost::hash_combine (mixing step)",
          upstream="https://www.boost.org",
          version="-",
          use="The one-line mixing step seed ^ (v + 0x9e3779b9 + (seed << 6) + "
              "(seed >> 2)) in src/core/shader/pipelinefactory.cpp, "
              "src/core/renderingpass/renderpipelinegpuarena.cpp and "
              "src/core/userpublic/details/ecs/coretemplate.hpp.",
          license="BSL-1.0",
          texts=[("deps:Catch2", "LICENSE.txt")],
          notes=["The license text below is the Boost Software License 1.0 as "
                 "it appears in Catch2's LICENSE.txt."]),
    Entry("SplitMix64 finalizer",
          upstream="Sebastiano Vigna, splitmix64.c (https://prng.di.unimi.it)",
          version="-",
          use="The finalizing mixer in src/project/targetplanning.cpp.",
          license="Public domain (as stated by its author; no text to reproduce)"),
    # --- External SDKs and runtimes ----------------------------------------
    Entry("Vulkan SDK 1.4.309.0 (shaderc_combined, glslang, Vulkan loader import library)",
          upstream="https://vulkan.lunarg.com",
          version="1.4.309.0",
          use="Release builds statically link shaderc_combined (shaderc, glslang "
              "and SPIRV-Tools) for the runtime shader compiler; Debug builds "
              "install shaderc_shared.dll. The executables link the Vulkan "
              "loader's import library; the loader DLL comes with the GPU "
              "driver and is not distributed. glslang also runs at build time.",
          license="per component (the SDK's LICENSE.txt below points to the "
                  "LunarG licensing registry)",
          texts=[("sdk", "Licenses/LICENSE.txt"),
                 ("repo", "third_party/licenses/shaderc-LICENSE"),
                 ("repo", "third_party/licenses/glslang-LICENSE.txt"),
                 ("repo", "third_party/licenses/Vulkan-Loader-LICENSE.txt")],
          notes=["The SPIRV-Tools entry's text is that of the version CMake "
                 "fetched, not necessarily of the one inside shaderc_combined.",
                 "The shaderc, glslang and Vulkan-Loader texts are their "
                 "upstream LICENSE files copied into third_party/licenses "
                 "(see its README.md), not files read from the SDK."]),
    Entry("Vulkan-Hpp / Vulkan-Headers",
          upstream="https://github.com/KhronosGroup/Vulkan-Hpp; "
                   "https://github.com/KhronosGroup/Vulkan-Headers",
          version="Vulkan SDK 1.4.309.0",
          use="C++ bindings and Vulkan API declarations compiled into the engine.",
          license="Apache-2.0 OR MIT (per file; vulkan_core.h is Apache-2.0)",
          texts=[("sdk", "Include/vulkan/vulkan.hpp", 1, 4),
                 ("sdk", "Include/vulkan/vulkan_core.h", 4, 8),
                 ("deps:battery-embed", "LICENSE")],
          notes=["Distributed under Apache-2.0. The full Apache-2.0 text below "
                 "is the same standard license included with battery-embed."]),
    Entry("Qt 6 (LGPL-3.0)",
          upstream="https://www.qt.io; source: https://download.qt.io/archive/qt/ "
                   "and https://code.qt.io",
          version="6.10.3 (the version the Studio build was configured with)",
          use="Pelican Studio links the Qt modules Core, Gui, Widgets, Network "
              "and Test dynamically. The Runtime install puts the unmodified "
              "Qt DLLs and plugins next to pelican_studio.exe (Qt's deploy "
              "script), so they can be replaced with another compatible build "
              "of Qt. Qt is used under the GNU Lesser General Public License "
              "version 3. Qt's source code for this version is available from "
              "the addresses above. The deployed plugins contain third-party "
              "parts of their own; Qt's SBOM files in the Qt installation "
              "(sbom/qtbase-<version>.spdx) list them.",
          license="LGPL-3.0-only (Qt open source); the LGPL-3.0 supplements the GPL-3.0",
          texts=[("repo", "third_party/licenses/lgpl-3.0.txt"),
                 ("deps:nlohmann", "LICENSES/GPL-3.0-only.txt")],
          notes=["The LGPL-3.0 text below is the FSF's text copied into "
                 "third_party/licenses (see its README.md); the GPL-3.0 text it "
                 "supplements is the standard text as it appears in "
                 "nlohmann/json's LICENSES/GPL-3.0-only.txt.",
                 "The non-Qt libraries Qt's deploy script copies next to "
                 "pelican_studio.exe have their own entry below."]),
    Entry("Libraries Qt's deploy script installs with Pelican Studio",
          upstream="ICU: https://icu.unicode.org; DirectX Shader Compiler: "
                   "https://github.com/microsoft/DirectXShaderCompiler; Mesa "
                   "(opengl32sw.dll, Qt's software OpenGL): https://mesa3d.org; "
                   "D3Dcompiler_47.dll: Microsoft",
          version="as shipped with Qt 6.10.3 for MSVC 2022 x64",
          use="Qt's deploy script copies icuuc.dll (ICU), dxcompiler.dll "
              "(DirectX Shader Compiler), opengl32sw.dll (a Mesa llvmpipe build) "
              "and D3Dcompiler_47.dll next to pelican_studio.exe for Qt's "
              "rendering back ends. They are unmodified and can be replaced.",
          license="ICU: Unicode License v3; DirectX Shader Compiler: University "
                  "of Illinois/NCSA Open Source License (with LLVM's terms); "
                  "Mesa: MIT (per file licenses listed by Mesa); "
                  "D3Dcompiler_47.dll: Microsoft redistributable code",
          texts=[("repo", "third_party/licenses/icu-LICENSE"),
                 ("repo", "third_party/licenses/DirectXShaderCompiler-LICENSE.TXT"),
                 ("repo", "third_party/licenses/mesa-license.rst"),
                 ("repo", "third_party/licenses/mesa-MIT")],
          notes=["The texts are the upstream files copied into "
                 "third_party/licenses (see its README.md), not files read "
                 "from the Qt installation; they are the current upstream "
                 "texts, not necessarily those of the exact builds Qt ships.",
                 "D3Dcompiler_47.dll is Microsoft redistributable code from the "
                 "Windows SDK, distributed under the Windows SDK's license "
                 "terms; there is no separate text to reproduce."]),
    Entry("Microsoft Visual C++ redistributable runtime",
          upstream="Microsoft",
          version="the installed MSVC toolset's",
          use="Release Runtime installs put the redistributable DLLs in bin "
              "(cmake/pelican_runtime_layout.cmake).",
          license="Microsoft Software License Terms (redistributable code)",
          missing=["the Visual Studio license terms that cover the redistributable files"]),
]

# Sources of methods (papers), not of code: listed so a reader can find where
# the formulas come from. Nothing to reproduce.
METHOD_REFERENCES = [
    ("src/core/resources/fullscreen.frag",
     "R. L. Cook, K. E. Torrance, 1982; B. Walter et al., EGSR 2007; "
     "C. Schlick, Computer Graphics Forum 1994; B. Karis, SIGGRAPH 2013 course"),
    ("src/core/resources/ssao.frag",
     "M. Mittring, SIGGRAPH 2007 course; D. Shanmugam, O. Arikan, I3D 2007"),
    ("src/core/resources/render_algorithms/planar_reflection/standard_prefilter.comp",
     "J. Jimenez, SIGGRAPH 2014 course (13-tap downsample)"),
    ("projects/pathtrace_demo/shaders/pt_bsdf.glsl",
     "E. Heitz, JCGT 7(4), 2018 (visible-normal sampling); B. Walter et al., EGSR 2007"),
    ("projects/pathtrace_demo/shaders/pt_rng.glsl",
     "M. Jarzynski, M. Olano, JCGT 9(3), 2020 (PCG as a GPU hash)"),
]

DECLARE = re.compile(r"FetchContent_Declare\(\s*([A-Za-z0-9_.+-]+)(.*?)\)", re.S)
REPOSITORY = re.compile(r"GIT_REPOSITORY\s+(\S+)")
TAG = re.compile(r"GIT_TAG\s+(\S+)([^\n]*)")


def cmake_files(repo: Path) -> list[Path]:
    skip = {"build", ".git", "experiments", "projects"}
    found = []
    for path in sorted(repo.rglob("*")):
        relative = path.relative_to(repo)
        if relative.parts and (relative.parts[0] in skip or relative.parts[0].startswith("build")):
            continue
        if "fixtures" in relative.parts:
            continue
        if path.is_file() and (path.name == "CMakeLists.txt" or path.suffix == ".cmake"):
            found.append(path)
    return found


def declarations(repo: Path) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for path in cmake_files(repo):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in DECLARE.finditer(text):
            name, body = match.group(1), match.group(2)
            repository = REPOSITORY.search(body)
            tag = TAG.search(body)
            comment = tag.group(2).strip().lstrip("#").strip() if tag else ""
            # Only the tag name a comment gives, not the prose after it.
            comment = re.split(r"[(（]", comment, maxsplit=1)[0].strip()
            result.setdefault(name, {
                "repository": repository.group(1) if repository else "",
                "tag": tag.group(1) if tag else "",
                "tag_comment": comment,
                "declared_in": [],
            })["declared_in"].append(path.relative_to(repo).as_posix())
    return result


def check_declarations(repo: Path) -> int:
    declared = set(declarations(repo))
    listed = {entry.fetch for entry in THIRD_PARTY if entry.fetch}
    status = 0
    for name in sorted(declared - listed):
        print(f"FetchContent_Declare({name}) has no THIRD_PARTY entry in "
              f"tools/third_party_notices.py", file=sys.stderr)
        status = 1
    for name in sorted(listed - declared):
        print(f"THIRD_PARTY lists '{name}', which no CMake file declares",
              file=sys.stderr)
        status = 1
    if status == 0:
        print(f"{len(declared)} FetchContent declarations match THIRD_PARTY")
    return status


def fence_for(text: str) -> str:
    longest = max((len(run) for run in re.findall(r"`+", text)), default=0)
    return "`" * max(3, longest + 1)


def read_text(entry: Entry, source: tuple, roots: dict[str, Path | None]) -> tuple[str, str]:
    root_name, relative = source[0], source[1]
    if root_name.startswith("deps"):
        fetch = root_name.split(":", 1)[1] if ":" in root_name else entry.fetch
        base = roots["deps"] / f"{fetch.lower()}-src" if roots["deps"] else None
        label = f"<build>/_deps/{fetch.lower()}-src/{relative}"
    elif root_name == "sdk":
        base = roots["sdk"]
        label = f"<Vulkan SDK>/{relative}"
    else:
        base = roots["repo"]
        label = relative
    if base is None or not (base / relative).is_file():
        raise FileNotFoundError(label)
    lines = (base / relative).read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
    if len(source) == 4:
        lines = lines[source[2] - 1:source[3]]
        label += f" (lines {source[2]}-{source[3]})"
    return label, "\n".join(lines).strip("\n")


def generate(repo: Path, deps: Path | None, sdk: Path | None) -> tuple[str, list[str]]:
    roots = {"repo": repo, "deps": deps, "sdk": sdk}
    declared = declarations(repo)
    not_acquired: list[str] = []
    out = [
        "# Third-party notices",
        "",
        "pelican2 builds in, ships next to, or derives code from the third-party "
        "software below. Each entry gives how it is used, its license, and the "
        "license text copied from the file named above it.",
        "",
        "This file is generated by `tools/third_party_notices.py` from the license "
        "files of the fetched dependencies, the vendored files and the Vulkan SDK; "
        "regenerate it rather than editing it. pelican2's own license is not part of "
        "this file.",
        "",
        "## Summary",
        "",
        "| Component | Version | License | Use |",
        "|---|---|---|---|",
    ]
    for entry in THIRD_PARTY:
        version = entry.version or ""
        if entry.fetch:
            info = declared.get(entry.fetch, {})
            version = info.get("tag", "")
            if info.get("tag_comment"):
                version += f" ({info['tag_comment']})"
        out.append(f"| {entry.name} | {version} | {entry.license} | "
                   f"{entry.use.split('. ')[0].rstrip('.')} |")
    out.append("")
    for entry in THIRD_PARTY:
        out += ["", f"## {entry.name}", ""]
        if entry.fetch:
            info = declared.get(entry.fetch, {})
            out.append(f"- Source: {info.get('repository', '')}")
            pinned = info.get("tag", "")
            if info.get("tag_comment"):
                pinned += f" ({info['tag_comment']})"
            out.append(f"- Pinned: `{pinned}` (FetchContent `{entry.fetch}`, "
                       f"{', '.join(info.get('declared_in', []))})")
        else:
            out.append(f"- Source: {entry.upstream}")
            out.append(f"- Version: {entry.version}")
        out.append(f"- Use: {entry.use}")
        out.append(f"- License: {entry.license}")
        for note in entry.notes:
            out.append(f"- Note: {note}")
        missing = list(entry.missing)
        for source in entry.texts:
            try:
                label, text = read_text(entry, source, roots)
            except FileNotFoundError as error:
                missing.append(f"{error} (file not found when this was generated)")
                continue
            fence = fence_for(text)
            out += ["", f"From `{label}`:", "", fence + "text", text, fence]
        for item in missing:
            out.append("")
            out.append(f"- Not acquired: {item}")
            not_acquired.append(f"{entry.name}: {item}")
    out += ["", "## Method references (no third-party code)", "",
            "These files implement published formulas or algorithms in their own "
            "code; the sources are cited in the files.", ""]
    for path, cites in METHOD_REFERENCES:
        out.append(f"- `{path}`: {cites}")
    if not_acquired:
        out += ["", "## Not acquired", "",
                "License texts this file should carry but that were not available "
                "when it was generated:", ""]
        out += [f"- {item}" for item in not_acquired]
    return "\n".join(out) + "\n", not_acquired


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--deps-dir", type=Path)
    parser.add_argument("--vulkan-sdk", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--check-declarations", action="store_true")
    args = parser.parse_args(argv)
    repo = args.repo_root.resolve()
    if args.check_declarations:
        return check_declarations(repo)
    deps = args.deps_dir or repo / "build" / "_deps"
    sdk = args.vulkan_sdk or (Path(os.environ["VULKAN_SDK"]) if os.environ.get("VULKAN_SDK") else None)
    output = args.output or repo / "THIRD_PARTY_NOTICES.md"
    if check_declarations(repo) != 0:
        return 1
    text, not_acquired = generate(repo, deps if deps.is_dir() else None, sdk)
    if args.check:
        current = output.read_text(encoding="utf-8") if output.is_file() else ""
        if current.replace("\r\n", "\n") != text:
            print(f"{output} is stale; run tools/third_party_notices.py", file=sys.stderr)
            return 1
        print(f"{output} is up to date")
        return 0
    output.write_bytes(text.encode("utf-8"))
    print(f"wrote {output} ({len(text.splitlines())} lines, "
          f"{len(not_acquired)} texts not acquired)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
