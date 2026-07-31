# WP101 Animation Graph Demo

This project is self-contained: `assets/animgraph_demo.gltf` embeds a small
skinned mesh and the `Walk`, `Run`, and `Jump` clips. Build the player with
`-DPELICAN_PROJECT=projects/animgraph_demo`, then run it with
`--project projects/animgraph_demo`.

The project inherits the engine `hybrid_v1` rendering preset and the standard
directional-shadow feature. Rendering improvements to that preset therefore
apply without copying its render targets and passes into this project.

- Hold `W`, `A`, `S`, or `D` to move the blend1d parameter from Walk to Run.
- Press and release `Space` while moving to interrupt into Jump and crossfade
  back to Move.
- The process log prints the current graph state and semantic pose hash.
