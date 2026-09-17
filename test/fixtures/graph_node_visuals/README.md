# graph_node_visuals

The graph document `<作業ディレクトリ>/playground` carries, in shipped form: two definitions
(`gain_curve`, `half_of`), an owned instance (`helmet_pulse`) whose `time` port
is connected and whose `gain` / `bias` ports stand on stored parameters, and a
second instance with no owner. Two of its nodes carry a `label`.

`devstudio_graphcanvas_scene_test` draws this document, so the node visuals are
taken on the same graph the user looks at in the real project rather than on a
miniature written for the test.

The fixture is read, never loaded: nothing here runs the engine.
