#include "vertbufcontainer.hpp"

namespace Pelican {

VertBufContainer::VertBufContainer(DependencyContainer &con) {}

ModelTemplate::PrimitiveRefInfo VertBufContainer::addPrimitiveEntry(CommonPolygonVertData &&data) {}
void VertBufContainer::removePrimitiveEntry(/* TODO */) {}
vk::Buffer VertBufContainer::getVertexBuffer(/* TODO */) const {}

} // namespace Pelican
