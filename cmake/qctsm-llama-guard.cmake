include_guard(GLOBAL)

function(qctsm_llama_build_targets directory output)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    qctsm_llama_build_targets("${child}" child_targets)
    list(APPEND targets ${child_targets})
  endforeach()
  set(${output} "${targets}" PARENT_SCOPE)
endfunction()

function(qctsm_guard_llama_source source_dir)
  cmake_parse_arguments(PARSE_ARGV 1 guard "" "" "SOURCE_TARGETS;BUILT_TARGETS;CONSUMER_TARGETS")
  set(verify_command "${CMAKE_COMMAND}"
    "-DQCOPILOTS_TOOL_SEMANTIC_MATCHING_LLAMA_SOURCE_DIR=${source_dir}"
    "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/qctsm-llama-verify.cmake")
  add_custom_target(qctsm-check-llama-source
    COMMAND ${verify_command}
    COMMENT "Checking pinned upstream llama.cpp sources" VERBATIM)
  # Attach to every upstream object/library/generator target. Guarding only a
  # consumer does not order its sibling library dependencies before compilation.
  foreach(target IN LISTS guard_SOURCE_TARGETS)
    add_dependencies("${target}" qctsm-check-llama-source)
  endforeach()
  add_custom_target(qctsm-verify-llama-source ALL
    COMMAND ${verify_command}
    DEPENDS ${guard_BUILT_TARGETS}
    COMMENT "Verifying unmodified upstream llama.cpp sources" VERBATIM)
  foreach(target IN LISTS guard_CONSUMER_TARGETS)
    add_dependencies("${target}" qctsm-verify-llama-source)
  endforeach()
endfunction()
