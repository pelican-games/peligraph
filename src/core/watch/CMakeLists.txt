target_sources(pelican_core PRIVATE
  contentdigest.cpp
  reloadgate.cpp
  reloadqueue.cpp
  filewatcher.cpp
  reloadservice.cpp
)
target_link_libraries(pelican_core PRIVATE picosha2)

