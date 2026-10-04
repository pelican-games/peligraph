# studio_gui_graph_order

The project `test/studio_scenarios/gui_graph_order.json` opens: a graph root
that selects no preset, whose rendering section holds three fullscreen passes
drawing with `engine://passthrough`. `left` and `right` read nothing (each
draws the gradient) into `left_color` and `right_color`; `present` reads both
and shows the first. Blank's rendering section has one pass, so Order... on a
pass of the rendering section is reached from the Graph here. Since engine
FO-1, `frame_node_order` reorders the rendering section's passes: `left` and
`right` depend on neither, so the scenario swaps them and the picture does not
change; `present` reads `left_color`, so an order that puts it before `left`
is refused with `frame_node_order.conflicts_dependency@1`.
