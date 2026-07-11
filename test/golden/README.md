# Golden image color semantics

`expected.png` is the color contract 2 terminal result: encoded-sRGB RGB bytes and straight,
untransferred alpha. PNG files intentionally contain no color-management chunks; every consumer
must interpret RGB as sRGB. The renderer and RPC capture paths use the same RGBA8 contract.

Golden updates are allowed only through the six-step procedure in
`docs/design_color_pipeline.md` section 3. `stem_fullscreen` and `vat_playback` retain their
pre-existing non-zero tolerances; all other cases use exact post-baseline comparison.
