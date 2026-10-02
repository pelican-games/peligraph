#pragma once

#include "../graphmodule.hpp"

#include <string>
#include <vector>

namespace Pelican {

// CV-1: pelican.canvas, the 2D layer stack. Authors write shapes, paints, a
// stack and an output on the common graph; none of these nodes runs. The
// lowering (canvaslowering.hpp) replaces them with render.image /
// render.attribute / render.buffer / render.scalar / render.raster nodes and
// a generated shader before anything evaluates the document.
//
// Both types are symbolic: no value flows on a geometry or a layer port, the
// lowering removes every edge that carries one.
//   pelican.canvas.geometry@1  shape.rect / shape.ellipse (CV-4: shape.text) -> shape.fill / shape.stroke
//   pelican.canvas.layer@1     shape.fill / shape.stroke / canvas.stack -> canvas.stack / canvas.output
//                              CV-2a: also canvas.image / canvas.from_image / canvas.color /
//                              canvas.transform, and a stack layer's `mask` names a layer port
//                              (a canvas.image) beside its `input`.
// canvas.from_image reads an image edge (`image`) of a render pass or render.image.
// CV-6a: canvas.place reads a layer, a colour image (`color`) and optionally a
// depth image (`depth`), and gives the colour image back with the stack drawn
// on it in 3D.
//
// A vector parameter a value edge may drive is split into one scalar port per
// component: `size.x`, `size.y`, `center.x`, `center.y`, `color.r` .. `color.a`.
// A component without an edge takes the number in `parameters`. A stack
// layer's opacity is the port `<input>.opacity`, beside the layer port
// `<input>` that `parameters.layers[].input` names.
std::vector<SemanticTypeSchema> builtinCanvasTypeSchemas();
void registerCanvasGraphModule(GraphModuleRegistry &registry);

// The operations the module registers, in name order.
const std::vector<std::string> &canvasGraphOperations();
bool isCanvasGraphOperation(std::string_view op);

// The scalar ports a canvas.stack node exposes beside its layer ports, one
// `<input>.opacity` per layer. Shared by the contract and the lowering.
std::string canvasStackOpacityPort(const std::string &input);
// CV-6a: `<input>.depth`, the scalar that may drive a layer's depth (read when
// the stack is placed by canvas.place, ignored by canvas.output).
std::string canvasStackDepthPort(const std::string &input);

} // namespace Pelican
