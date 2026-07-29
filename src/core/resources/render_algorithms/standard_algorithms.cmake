# The standard package is intentionally declarative. Each algorithm owns its
# sources below this directory; the engine registry consumes only this list.
# Projects that disable PELICAN_WITH_STANDARD_RENDER_ALGORITHMS retain the
# generic graph/compiler/provider machinery and can supply project shader and
# source-level ViewFamily implementations.
set(PELICAN_STANDARD_RENDER_ALGORITHM_RESOURCES
  render_algorithms/planar_reflection/standard_prefilter.comp
)

foreach(resource_id IN LISTS PELICAN_STANDARD_RENDER_ALGORITHM_RESOURCES)
  b_embed(pelican_resources ${resource_id})
  list(APPEND PELICAN_OPTIONAL_ENGINE_RESOURCE_IDS ${resource_id})
endforeach()
