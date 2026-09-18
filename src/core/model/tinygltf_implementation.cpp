// The single translation unit that carries the tinygltf and stb
// implementations. Every other file includes <tiny_gltf.h> for declarations
// only; the implementation block of that header sits outside its include
// guard, so defining these in a file that reaches the header twice (gltf.cpp
// does, through gltfaccessorreader.hpp) compiles it twice.
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include <tiny_gltf.h>
