if(PELICAN_WITH_RPC)
target_sources(pelican_core PRIVATE
    rpcserver.cpp
    windowedrpchost.cpp
)
else()
target_sources(pelican_core PRIVATE
    rpcserver_stub.cpp
)
endif()

target_sources(pelican_core PRIVATE
    editorassetqueryruntime.cpp
    editorcommandservice.cpp
    editorjournal.cpp
    editorpreviewservice.cpp
    editorruntimefactory.cpp
)
