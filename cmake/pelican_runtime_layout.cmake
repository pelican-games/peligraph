# Keep each build/configuration self-contained. Two build trees must never
# overwrite one another's player, tools, or optional SDK staging output.
set(PELICAN_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/dist" CACHE PATH
    "Root for Pelican application and optional SDK build outputs")
set(PELICAN_INSTALL_DIR "${PELICAN_OUTPUT_ROOT}/$<CONFIG>")
set(PELICAN_LIB_DIR "${PELICAN_INSTALL_DIR}/lib/bin")
set(PELICAN_INCLUDE_DIR "${PELICAN_INSTALL_DIR}/lib/include")

function(pelican_install_compiler_runtime)
    if(MSVC)
        # Use the compiler's redistributable inventory for both Studio and
        # player-only bundles. Debug CRTs are development dependencies.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
        include(InstallRequiredSystemLibraries)
        if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                DESTINATION bin COMPONENT Runtime
                CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
        else()
            message(WARNING "MSVC redistributable libraries were not found; the Runtime package needs the Visual C++ runtime installed on its host")
        endif()
    endif()
endfunction()
pelican_install_compiler_runtime()

function(pelican_runtime_target target_name)
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PELICAN_INSTALL_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${PELICAN_INSTALL_DIR}")
    install(TARGETS ${target_name}
        RUNTIME DESTINATION bin COMPONENT Runtime
        LIBRARY DESTINATION bin COMPONENT Runtime
        ARCHIVE DESTINATION lib COMPONENT Development EXCLUDE_FROM_ALL)
    set_property(GLOBAL APPEND PROPERTY PELICAN_RUNTIME_TARGETS ${target_name})
endfunction()
