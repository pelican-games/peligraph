# studio_automation_graph

The project the Studio scenarios (`test/studio_scenarios/*.json`) open. It is a
copy of `graph_root_example`'s rendering document with the common-graph shape of
`graph_instance_inputs` authored into it: one definition, and a root that holds
one value source and one owned instance of that definition.

The scenarios copy this directory to a temp directory before opening it --
`graph_edit_roundtrip` writes the document back through the engine, and a test
that edits a file in the repository is a test that only passes once.

Counts the scenarios assert, and where they come from:

* the root scope has **2** nodes (`clock`, `left`), so the canvas draws two
  `graph_node` items and `get_graph_instances` answers with two;
* there is **1** definition (`gain_curve`);
* `left` stores `gain` = 2.5 and `bias` = 5 and connects neither, so both ports
  show an editable pill rather than an edge.
