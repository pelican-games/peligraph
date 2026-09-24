# studio_fullscreen_pass_from_template

The project `test/studio_scenarios/fullscreen_pass_from_template.json` opens.
It is `studio_automation_graph` under its own name. The scenario adds a
fullscreen pass after `lighting_pass` that reads `gbuffer_albedo` and writes
`lit_color`, the colour the bloom composite puts on the screen, so what the
pass's shader draws is what the viewport shows. (F-3 first gave the pass a
render target of its own, `f3_template_output`; nothing presented it, so F-5,
which checks that a saved shader changes the viewport, points the pass at
`lit_color` and the target is gone.)

The scenario writes `shaders/f3_tinted.frag` into its temp copy of this
directory with the Pass Editor's "New...", breaks and repairs it from the
driver, and saves the pass through `add_authored_pass`. It then edits the file
the way an author's own editor does -- a gradient, a typo, the fix -- and
Studio asks the engine to reload its shaders after each write. The copy here
has no `shaders/` folder: the scenario checks that "New..." makes one.
