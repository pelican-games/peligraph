# VI-3b: writes one engine shader's bytes into a C++ source file, so Studio
# carries the same file the engine compiles (no copy of its text is kept in
# the repository). Run as a script:
#   cmake -DINPUT=<shader> -DOUTPUT=<.cpp> -DSYMBOL=<C++ name> -P <this file>
# The file is read as bytes, so the copy Studio writes into a project is
# byte for byte the engine's.
if(NOT INPUT OR NOT OUTPUT OR NOT SYMBOL)
    message(FATAL_ERROR "embed_engine_shader: INPUT, OUTPUT and SYMBOL are required")
endif()
file(READ "${INPUT}" bytes HEX)
string(LENGTH "${bytes}" digits)
math(EXPR size "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," array "${bytes}")
set(content "// Generated from ${INPUT} by embed_engine_shader.cmake. Do not edit.\n")
string(APPEND content "#include <cstddef>\n\nnamespace PelicanStudio {\n")
string(APPEND content "extern const unsigned char ${SYMBOL}Bytes[] = {${array}0x00};\n")
string(APPEND content "extern const std::size_t ${SYMBOL}Size = ${size};\n")
string(APPEND content "} // namespace PelicanStudio\n")
file(WRITE "${OUTPUT}.tmp" "${content}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
