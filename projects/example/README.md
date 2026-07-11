# Pelican Example Project

This project is the default local example for `pelican_player --project`.

`shaders/toon.surface` and `materials/toon.material.json` are the B-layer
source-material example. The surface declares its interface and versioned
hooks; the material only references it and overrides authored values.

Binary assets are intentionally not tracked in git. Their expected project-relative inventory is recorded in
`assets.manifest.json`. After cloning, use the manifest-aware commands to see where the asset store resolves and
which files still need to be supplied:

```sh
pelican_cli assets status --project projects/example
pelican_cli assets verify --project projects/example
```

Use `assets verify --full` when content corruption is suspected. After an intentional asset update, regenerate the
sorted manifest and review its git diff before committing:

```sh
pelican_cli assets verify --full --project projects/example
pelican_cli assets manifest --project projects/example
```

Large binary assets should normally use Git LFS or an external asset store configured through
`.pelican/local.json`. The manifest records content and relative structure; the local override changes only the
resolved store location.
