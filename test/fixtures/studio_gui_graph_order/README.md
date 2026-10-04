# studio_gui_graph_order

The project `test/studio_scenarios/gui_graph_order.json` opens: a graph root
that selects no preset, whose rendering section holds three fullscreen passes
drawing with `engine://passthrough`. `left` and `right` read nothing (each
draws the gradient) into `left_color` and `right_color`; `present` reads both
and shows the first. Blank's rendering section has one pass, so Order... on a
pass of the rendering section is reached from the Graph here. The engine
chains a canonical bucket's passes in their declared order, so the order the
scenario applies is the one the plan already runs; the picture does not change.
