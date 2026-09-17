# Pelican package template

This directory is a complete Pelican package. Copy it anywhere outside the
engine tree, rename the namespace, and it is yours: nothing here refers to a
path inside the engine repository, and nothing in the engine looks for it until
a project installs it.

```
package.json          the pelican.package manifest
features/glow.json    one render feature (engine shaders only)
graphs/pulse.json     one graph document with a reusable definition
code/                 one game system, compiled against the public SDK
```

## Install it into a project

```sh
pelican_cli package install <this directory> --project <project directory>
```

The install copies the whole directory to `<project>/packages/acme.glowkit/`
and records `{namespace, package_version, sha256}` in `project.json.packages`.
Every check -- manifest shape, `requires.engine_min_version`,
`requires.sdk_abi`, installed dependencies, the shape of every provided feature
document -- runs before a byte is copied.

Each provided feature document is rewritten on the way in so its `name` carries
the namespace: `glow` becomes `acme.glowkit.glow`. That is the name
`list_render_features` reports, the name feature composition dedupes on, and
the name the shader define prefix is built from
(`PELICAN_FEATURE_ACME_GLOWKIT_GLOW_`).

## Use what it brings

A render feature is referenced from the project's rendering config by its
vendored path:

```json
"features": ["project://packages/acme.glowkit/features/glow.json"]
```

A graph definition is not referenced across documents in v0; copy the
definition you want out of `graphs/pulse.json` into the project's own graph.

## Build the code

The host project's `code/CMakeLists.txt` needs one line, after its own
`pelican_game_sources(...)`:

```cmake
pelican_game_sources(game.cpp)
pelican_packages()
```

`pelican_cli project init` writes both lines already. Then configure the player
against the project as usual:

```sh
cmake -S <engine> -B <build> -DPELICAN_PROJECT=<project>
cmake --build <build>
```

`pelican_packages()` finds every `packages/*/code/CMakeLists.txt`, and
`pelican_package_sources(...)` compiles those sources with the public SDK as
their only engine include path. The objects join the project's game logic DLL;
a package never produces a DLL of its own.

## Remove it

`uninstall_package{namespace}` over RPC removes the directory and the
`project.json` entry. It refuses with `package_in_use` while the rendering
config or its preset still references anything under
`project://packages/acme.glowkit/`.
