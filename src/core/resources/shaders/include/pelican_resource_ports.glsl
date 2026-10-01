#ifndef PELICAN_RESOURCE_PORTS_GLSL
#define PELICAN_RESOURCE_PORTS_GLSL

// RS-11: the resource-port include of a pass that maps no ports.
//
// A fullscreen / raster pass with `resource_ports` (a graph render node's
// `ports`) compiles its shaders with a generated pelican_resource_ports.glsl in
// place of this file: it declares each port's descriptor and accessors
// (pelican_load_<port>, pelican_sample_<port>, ...) and defines
// PELICAN_RESOURCE_PORT_<port> for each. A pass that maps none reaches this
// empty one, so a shader whose ports are optional tests the define and reads
// a default without the port (engine://bloom_composite's bloomIntensity).
// A shader that calls an accessor without such a test still fails to compile
// by the accessor's name when its pass leaves the port out.

#endif
