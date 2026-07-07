if(PELICAN_WITH_RPC)
target_sources(pelican_core PRIVATE
    jsonrpc.cpp
    rpcserver.cpp
)
else()
target_sources(pelican_core PRIVATE
    rpcserver_stub.cpp
)
endif()
