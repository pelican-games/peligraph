target_sources(pelican_core PRIVATE
  assetkey.cpp
  contentdigest.cpp
  reloadgate.cpp
  reloadqueue.cpp
  reloadtransaction.cpp
  filewatcher.cpp
  reloadservice.cpp
)
target_link_libraries(pelican_core PRIVATE picosha2)

