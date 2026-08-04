#version 460

layout(location = 0) flat in uint pickingId;
layout(location = 0) out uint outPickingId;

void main() {
    outPickingId = pickingId;
}
