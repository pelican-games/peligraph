# graph_instance_inputs

A rendering document in the `pelican.graph` form with one sub-graph definition
and one owned attachment, used by the tests for instance input supply (mode,
value and origin).

None of the five shipped projects has a `pelican.graph` document, so the
acceptance for that work is taken here instead. Moving a shipped project to the
graph form is the learning-sample work, not this one.

* `gain_curve` declares two inputs. `gain` has a default; `bias` has none, so
  the attachment must store a value for it -- which is what makes "remove the
  edge and the port goes back to its stored value" a real question rather than
  a hypothetical one.
* `left` stores both values and connects neither, so a test can drive it in
  either direction from a known start.
* `clock` is the scene clock a curve attaches to, already present so
  "control with a curve" exercises the reuse path rather than always adding
  its own time source.
