foreach(required GLSLC_EXECUTABLE SHADER_TARGET_ENV INPUT OUTPUT HASH_OUTPUT)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "compile_wp357_shader.cmake requires ${required}")
  endif()
endforeach()

get_filename_component(output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
execute_process(
  COMMAND "${GLSLC_EXECUTABLE}"
          "--target-env=${SHADER_TARGET_ENV}"
          -o "${OUTPUT}"
          "${INPUT}"
  RESULT_VARIABLE compile_result
  OUTPUT_VARIABLE compile_stdout
  ERROR_VARIABLE compile_stderr
)
if(NOT compile_result EQUAL 0)
  message(FATAL_ERROR
    "WP357 fixture compilation failed (${compile_result})\n"
    "${compile_stdout}${compile_stderr}")
endif()

file(SHA256 "${INPUT}" source_sha256)
file(WRITE "${HASH_OUTPUT}" "${source_sha256}\n")
