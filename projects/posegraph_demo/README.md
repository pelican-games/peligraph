# Pose Graph Demo

One object, two clips, and a curve that decides how much of each you see. The
whole thing is data: there is no `code/` directory here, so everything below is
something you can change with a text editor and see in the next run.

    pelican_player --project projects/posegraph_demo

This project needs a build with `PELICAN_WITH_ANIMATION=ON`. It says so in
`project.json` (`"requires": ["animation"]`), and a build without that module
refuses to open it by name -- `animation.module_unavailable@1` -- instead of
failing later on an operation nobody registered.

## Read it in this order

1. **`project.json`** -- the shell. `rendering_config_json` points at
   `passes/main.json`, and `requires` names the build feature above.
2. **`scene.json`** -- two objects. `Hero` is **first** and `Camera` is second.
   That order is load-bearing; see "the owner is a position, not a name" below.
   `Hero` carries `transform` and `simplemodelview`. It does not declare a
   `localtransform`: the engine gives every object with a transform one at
   load, holding the same TRS, so nothing moves when the local transform
   system recomposes it.
3. **`passes/main.json`** -- the rendering config, written as a `pelican.graph`
   root. Three parts:
   - `definitions.walk_run` -- the reusable piece. A `value.input` reads
     `scene_time`; two `pose.clip_sample` nodes sample `Walk` and `Run` from the
     same rig at that time; `pose.blend` mixes them; `pose.skeletal` hands the
     result to whichever object the instance owns. `weight` is a `graph.input`,
     which is this definition's one knob.
   - the root `nodes` -- `clock` (`scene_time` again), `cycle`
     (`value.playback`, 4 s, `wrap: repeat`) and `blend_curve` (`value.curve`),
     then `hero_walk_run`, the `graph.instance` that runs `walk_run` on the
     object named by `owner`.
   - `rendering` -- the ordinary render config (the `hybrid_v1` preset plus the
     directional-shadow, sky-ambient and UI features), moved under the graph
     root's `rendering` key. Nothing about rendering changed by adopting a graph
     root; only the graph above it is new.
4. **`assets/posegraph_demo.gltf`** -- a copy of the animgraph demo's rig: three
   nodes (`DemoMesh`, `RootJoint`, `TipJoint`) and three clips. `Walk` and `Run`
   both rotate `TipJoint` about z over exactly 1 second, with keys at 0, 0.25,
   0.5, 0.75 and 1.0 s. **`Walk` swings +-20 degrees and `Run` +-50 degrees**
   (the glTF stores half-angles in the quaternion's z, so the numbers in the
   file look half as large as the motion). At t = 0, 0.5 and 1.0 s both clips
   are at rest, so those moments tell you nothing about the blend; a quarter
   second in is where you can see it.

## What to change, and what happens

**The weight.** `pose.blend` takes `a` at weight 0 and `b` at weight 1, and
clamps everything outside. Here `a` is `Walk` and `b` is `Run`, so weight 0 is a
pure walk, weight 1 a pure run, and 0.5 the halfway posture. At t = 0.25 s the
joint is at 20 degrees for weight 0, 35 degrees for weight 0.5 and 50 degrees
for weight 1 -- the blend is a shortest-arc mix, so the midpoint really is the
midpoint of the angle.

**The curve.** `cycle` turns `scene_time` into a position inside a 4-second
loop (`source_begin`/`source_end` make that position read in seconds, so the
curve's key times are seconds too), and `blend_curve` maps it to a weight:
0 at the start, 1 half a second in, and back to 0 by the end of the loop. Move
the keys and the individual surges into a run sooner, later, or not at all.

**Taking the curve away, and putting it back.** The instance keeps
`"parameters": {"weight": 0.0}` underneath the edge. An input port takes the
edge if there is one, then the parameter, then the definition's default -- so
disconnecting the edge does not leave the port empty, it leaves it at 0.0 and
the hero walks. In Studio's Inspector that is the "disconnect" action on the
port; over the editor RPC it is one `edit_graph_document` operation:

    {"op":"set_input","node":"hero_walk_run","port":"weight","source":null}

and connecting it again is the same operation with
`{"node":"blend_curve","port":"value"}` as the source. Changing the value the
port falls back to is `set_node_parameters` on `hero_walk_run` with
`{"weight": 1.0}`. Note that the Inspector's own "make this a curve" action
builds a *flat* two-key curve at the value the port has right now -- on purpose,
so that switching modes never moves the number under you. It does not restore
this project's curve; to get it back, rewrite the keys (or `git checkout` this
file).

**The owner is an authoring id, not a name.** `owner: {"scene": "default_scene",
"object": 1}` names the object whose authoring id is 1. This `scene.json` has
never been saved by the editor, so it carries no ids yet: they are handed out by
position when the file loads, starting at 1, and id 1 is `Hero`, the first
object in the `objects` array. While the file stays that way, move `Camera` to
the front by hand, or insert an object above `Hero`, and the instance owns
something that has no model (what is reported then is in manual chapter 4, under
`graph.instance.owner`). Saving the scene from the editor (`save_scene`) writes
every object's `authoring_object_id` and the document's
`next_authoring_object_id` into the file; from then on an id travels with its
object, so objects inserted or reordered before `Hero` no longer move the owner,
and reopening the saved file keeps the same ids. The one exception is a scene
snapshot import (`import_scene_snapshot`), which allocates fresh ids.

**The clips.** `parameters.clip` on either `pose.clip_sample` node takes any
clip name in the rig; `Jump` is the third one here. `pose.clip_sample` also
carries `speed`, `start_time` and `loop`, the same three playback parameters the
animation component has, so one clip means one motion however it is attached.

**Root motion** is not part of this demo: `Jump` returns exactly where it
started (a loop-closed hop has no net displacement), and an extracting node may
not sit upstream of `pose.blend` -- that is `pose.root_motion_unsupported@1`.
For what root motion does and where it lands, read the AG-2 tests in
`test/skeletal_rootmotion_test.cpp`.

## Where this project is checked

`project_catalog_headless_smoke` runs it headless for ten frames and requires
the last frame to differ from the first, because a rendering config with `pose.`
operations in it is a declaration that the project moves. The blend weight
itself is measured in `skeletal_gltf_test` ("the posegraph_demo curve drives the
blend that poses the hero"), which loads this document and this scene, adds a
GPU probe, and reads the posed joint back at t = 0.25 s.
