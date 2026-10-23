# studio_gui_graph_task_delete

The project `test/studio_scenarios/gui_graph_task_delete.json` opens (GL-5f,
decision 2). It is `studio_fullscreen_pass_from_template` with one feature the
project wrote: `features/light_select.json`, named `light_select`, whose one
compute task `light_select` runs the engine's clustered light selection shader
before `lighting_pass` (the buffers are the ones `clustered_lighting` declares,
under the feature's own names). The Graph tab draws the feature as a folded
node, `feature:light_select`; inside it the task is a node of its own, and the
scenario deletes it there (Delete task: remove_compute_task, the Frame Plan's
Delete).
