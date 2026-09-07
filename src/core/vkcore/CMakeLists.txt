target_sources(pelican_core PRIVATE
    bootstrap.cpp
    core.cpp
    debugutils.cpp
    devicefeaturepolicy.cpp
    memorydiagnostics.cpp
    deletionqueue.cpp
    externaldepthsubmission.cpp
    frametarget.cpp
    outputcompilefacts.cpp
    swapchainrecovery.cpp
    windowsurface.cpp
    util.cpp
    cmdbuf.cpp
    offscreenframetarget.cpp
    previewexecutor.cpp
    rendertarget.cpp
    swapchainframetarget.cpp
    render_pass_dispatch.cpp
    render_pass_executor.cpp
    render_pass_frame_setup.cpp
    render_target_layout_tracker.cpp
    rendertiming.cpp
    renderer.cpp
    renderer_config.cpp
    shaderresolution.cpp
    vulkanvalidation.cpp
    vma_impl.cpp
)

# This translation unit exceeds COFF's 16-bit section count in MSVC Debug.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/renderer.cpp"
    TARGET_DIRECTORY pelican_core APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")
