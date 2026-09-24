# studio_fullscreen_pass_from_template

The project `test/studio_scenarios/fullscreen_pass_from_template.json` opens.
It is `studio_automation_graph` with one more render target declared in the
root's `rendering.render_targets`: `f3_template_output`, which nothing writes
or reads yet. The scenario adds a fullscreen pass that writes it, so the new
pass has a colour target of its own instead of a second writer of one the
graph already uses.

The scenario writes `shaders/f3_tinted.frag` into its temp copy of this
directory with the Pass Editor's "New...", breaks and repairs it from the
driver, and saves the pass through `add_authored_pass`. The copy here has no
`shaders/` folder: the scenario checks that "New..." makes one.
