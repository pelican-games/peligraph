target_sources(pelican_core PRIVATE
    core.cpp
    deletionqueue.cpp
    util.cpp
    cmdbuf.cpp
    offscreenframetarget.cpp
    rendertarget.cpp
    swapchainframetarget.cpp
    render_pass_dispatch.cpp
    render_pass_executor.cpp
    render_pass_frame_setup.cpp
    render_target_layout_tracker.cpp
    rendertiming.cpp
    renderer.cpp
    renderer_config.cpp
    vma_impl.cpp
)
