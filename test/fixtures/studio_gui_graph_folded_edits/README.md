# studio_gui_graph_folded_edits

The project `test/studio_scenarios/gui_graph_folded_edits.json` opens (GL-5e):
a graph root that selects no preset, with two things Blank cannot make.

- `passes/authoring/pass-0123456789abcdef0123.json` is a pass the project
  authored -- a managed fragment, as the Pass Editor's Save of a new pass
  writes one (the `pelican_editor_managed` marker), listed in the root's
  `features` -- whose fullscreen `tint` draws `tint_color` before present.
  The Graph tab draws it as a folded node,
  `feature:authored_pass_0123456789abcdef0123`, and the scenario deletes it
  there (remove_authored_pass).
- `features/cube_capture.json` is a feature named `cube_capture` that declares
  only the six-face `cube_capture_color` and no pass: a feature of that name is
  what makes the engine's standard cube capture provider build the
  `$capture/cube` view family, with its `face` axis. `face_tint`, a pass of
  the root's rendering section, draws into that target in that family, so its
  detail column on the Graph has a View field; the scenario chooses face 2.

`present` reads nothing and draws `engine://passthrough`'s gradient: neither
edit changes the picture. The scenario copies this directory before opening it;
nothing here is written.
