target_sources(pelican_core PRIVATE
    vertbufcontainer.cpp
    modeltemplatecontainer.cpp
    gltf.cpp
)

FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG        v2.9.7
)
FetchContent_MakeAvailable(tinygltf)

target_link_libraries(pelican_core PRIVATE tinygltf)
