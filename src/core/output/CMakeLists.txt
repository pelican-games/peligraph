target_sources(pelican_core PRIVATE
    outputsinks.cpp
    outputfile.cpp
    outputslotqueue.cpp
    outputvideo.cpp
    outputvideoprocess.cpp
    outputspout.cpp
    outputspoutgpupath.cpp
)

# VO-4: only the seven sources spoutDX needs, from the pinned and patched tag.
# Nothing here exists with the option off, so nothing links d3d11 or dxgi then.
if(PELICAN_WITH_SPOUT)
  set(spout_sdk "${spout2_SOURCE_DIR}/SPOUTSDK")
  add_library(pelican_spoutdx STATIC
      "${spout_sdk}/SpoutDirectX/SpoutDX/SpoutDX.cpp"
      "${spout_sdk}/SpoutGL/SpoutCopy.cpp"
      "${spout_sdk}/SpoutGL/SpoutDirectX.cpp"
      "${spout_sdk}/SpoutGL/SpoutFrameCount.cpp"
      "${spout_sdk}/SpoutGL/SpoutSenderNames.cpp"
      "${spout_sdk}/SpoutGL/SpoutSharedMemory.cpp"
      "${spout_sdk}/SpoutGL/SpoutUtils.cpp"
  )
  set_target_properties(pelican_spoutdx PROPERTIES
      CXX_STANDARD 17
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  target_compile_options(pelican_spoutdx PRIVATE /w)
  target_include_directories(pelican_spoutdx SYSTEM PUBLIC
      "${spout_sdk}/SpoutDirectX/SpoutDX"
      "${spout_sdk}/SpoutGL")
  target_link_libraries(pelican_spoutdx PRIVATE
      d3d11 dxgi shell32 shlwapi advapi32 version)
  target_sources(pelican_core PRIVATE outputspoutbackend_win.cpp outputspoutgpusource_win.cpp)
  target_link_libraries(pelican_core PRIVATE pelican_spoutdx dxgi)
endif()
