# studio_lighting_pass_template

The project `test/studio_scenarios/lighting_pass_template.json` opens. Its
rendering document is `studio_fullscreen_pass_from_template`'s -- a G-buffer
pass, the engine's own `lighting_pass` into `lit_color`, and the bloom
composite that puts `lit_color` on the screen. Its scene is not empty: one box
(`assets/ground.glb`, a copy of `test/fixtures/ground.glb`), turned so three of
its faces are seen, and one directional light.

The scenario adds a fullscreen pass after `lighting_pass` that reads the five
G-buffer targets and writes `lit_color`, with the Pass Editor's "New..." and
its Deferred lighting starting point. What the viewport shows is then what that
shader draws: the box lit by the scene's light over a black background. With
the light loop of the shader cut, it is black everywhere, which is the check
that the light reaching the picture is the scene's.
