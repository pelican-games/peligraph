target_sources(pelican_core PRIVATE
    modelresourcerelease.cpp
    vertbufcontainer.cpp
    engineprimitives.cpp
    gltf.cpp
    gltfaccessorreader.cpp
    tinygltf_implementation.cpp
    gltfimage.cpp
    modeltemplate.cpp
    occurrenceidentity.cpp
    skeletalanimation.cpp
    vrmaanimation.hpp
    vrmfirstperson.cpp
    vrmsemantic.cpp
)
if(PELICAN_WITH_VAT)
target_sources(pelican_core PRIVATE
    vatformat.cpp
)
endif()

FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG        488a70a3df62a4df1a736e9e56fb8836580c4888 # v2.9.7
)
FetchContent_MakeAvailable(tinygltf)

target_link_libraries(pelican_core PRIVATE tinygltf)
if(WIN32)
target_link_libraries(pelican_core PRIVATE Normaliz)
endif()
