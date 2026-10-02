add_library(pelican_schema_core_adapter STATIC
    schemavocabularyadapter.cpp
    schemavocabularyadapter.hpp
)
set_target_properties(pelican_schema_core_adapter PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
target_include_directories(pelican_schema_core_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pelican_schema_core_adapter PUBLIC pelican_schema_leaf)
if(MSVC)
    target_compile_options(pelican_schema_core_adapter PRIVATE
        /W4 /we4061 /we4062)
else()
    target_compile_options(pelican_schema_core_adapter PRIVATE
        -Wswitch-enum -Werror=switch-enum)
endif()
target_link_libraries(pelican_core PRIVATE pelican_schema_core_adapter)

if(PELICAN_WITH_RPC)
target_sources(pelican_core PRIVATE
    rpcserver.cpp
    playsession.cpp
    windowedrpchost.cpp
    editorrpchandlers.cpp
    projectmigrationrpc.cpp
)
else()
target_sources(pelican_core PRIVATE
    rpcserver_stub.cpp
)
endif()

target_sources(pelican_core PRIVATE
    animationstatus.cpp
    audiostatus.cpp
    editorassetqueryruntime.cpp
    proceduralasseteditor.cpp
    proceduralassetpublication.cpp
    editorcommandservice.cpp
    editorjournal.cpp
    editorjournalintegrity.cpp
    editorjournalops.cpp
    editorpreviewservice.cpp
    editorprojectionplan.cpp
    engineauthoringlists.cpp
    renderconfigeditor.cpp
    renderconfigaudioanalysis.cpp
    graphfoldverify.cpp
    editorruntimefactory.cpp
)
