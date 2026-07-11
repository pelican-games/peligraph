target_sources(pelican_core PRIVATE
  types.cpp
  widgetarena.cpp
  commandbuffer.cpp
  drawcommands.cpp
  document.cpp
  layout.cpp
  inputrouter.cpp
  semanticfixture.cpp
)

# The layout contract is IEEE-754 binary64 with RN-even and no contraction.
if(MSVC)
  set_source_files_properties(types.cpp layout.cpp PROPERTIES COMPILE_OPTIONS "/fp:strict")
else()
  set_source_files_properties(types.cpp layout.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-frounding-math")
endif()
