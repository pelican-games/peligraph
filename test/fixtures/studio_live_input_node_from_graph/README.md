# studio_live_input_node_from_graph

OSC-3a's scenario fixture: `studio_automation_graph` plus one declared live
input, `a` (float, 0..1, default 0), in `live_inputs.json`, named by
`basic_config.live_inputs_json`.

The scenario copies this directory to a temp directory before opening it. The
declaration is what makes `get_live_inputs` answer `a`, which the Graph canvas
shows to the value.input dialog as `live:a`. The scenario places that node
through the palette and the required-parameter dialog, then reads the engine's
document back.

Selection explains the `live:` prefix: the RPC reply's `name` is the declared
name (`a`); a value.input node's `parameters.name` is `live:a`.
