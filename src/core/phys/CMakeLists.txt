if(PELICAN_WITH_PHYSICS)
  target_sources(pelican_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/physquerycontract.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/physicsruntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/physicsservice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/physworld.cpp
  )
  if(PELICAN_WITH_BUILTIN_PHYSICS)
    target_sources(pelican_core PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/physquery.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/physquerysweep.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/physqueryaggregate.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/builtinphysicsprovider.cpp
    )
  endif()
  if(PELICAN_WITH_JOLT_PHYSICS)
    target_sources(pelican_core PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/joltphysicsprovider.cpp
    )
  endif()
else()
  target_sources(pelican_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/physicsservice_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/physworld_stub.cpp
  )
endif()
