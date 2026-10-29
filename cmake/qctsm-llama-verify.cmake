# Verify the unmodified upstream archive or Git checkout against the same lock.
# This module never writes into the dependency source directory.
include_guard(GLOBAL)

function(qctsm_verify_llama_source source_dir)
  if(IS_SYMLINK "${source_dir}" OR NOT IS_DIRECTORY "${source_dir}")
    message(FATAL_ERROR "The llama.cpp source must be an existing directory, not a symlink: ${source_dir}")
  endif()
  get_filename_component(_project_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  file(READ "${_project_root}/dependencies.lock.json" _lock)
  string(JSON _tag GET "${_lock}" "llama.cpp" tag)
  string(JSON _locked_commit GET "${_lock}" "llama.cpp" commit)
  string(JSON _manifest_path GET "${_lock}" "llama.cpp" sourceManifest)
  string(JSON _locked_manifest_hash GET "${_lock}" "llama.cpp" sourceManifestSha256)
  set(_manifest "${_project_root}/${_manifest_path}")
  file(SHA256 "${_manifest}" _manifest_hash)
  if(NOT _manifest_hash STREQUAL _locked_manifest_hash)
    message(FATAL_ERROR "The llama.cpp source manifest differs from its reviewed hash")
  endif()

  if(EXISTS "${source_dir}/.git")
    # Worktrees have a .git file. An unrelated enclosing repository must never
    # authenticate an archive, and a matching HEAD does not excuse modified files.
    if(NOT GIT_EXECUTABLE)
      find_package(Git REQUIRED)
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse --show-toplevel
      OUTPUT_VARIABLE _git_root OUTPUT_STRIP_TRAILING_WHITESPACE ENCODING UTF-8 COMMAND_ERROR_IS_FATAL ANY)
    file(REAL_PATH "${source_dir}" _source_root)
    file(REAL_PATH "${_git_root}" _git_root)
    if(WIN32)
      string(TOLOWER "${_source_root}" _source_root)
      string(TOLOWER "${_git_root}" _git_root)
    endif()
    if(NOT _source_root STREQUAL _git_root)
      message(FATAL_ERROR "The llama.cpp Git checkout root differs from the requested source directory")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
      OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ENCODING UTF-8 COMMAND_ERROR_IS_FATAL ANY)
    if(NOT _commit STREQUAL _locked_commit)
      message(FATAL_ERROR "llama.cpp ${_tag} does not match the locked commit")
    endif()
  endif()

  if(NOT Python3_EXECUTABLE)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/qctsm-verify-llama-source.py" --source "${source_dir}"
    RESULT_VARIABLE _verified OUTPUT_VARIABLE _output ERROR_VARIABLE _error ENCODING UTF-8)
  if(NOT _verified EQUAL 0)
    message(FATAL_ERROR "${_output}${_error}")
  endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED QCOPILOTS_TOOL_SEMANTIC_MATCHING_LLAMA_SOURCE_DIR)
    message(FATAL_ERROR "QCOPILOTS_TOOL_SEMANTIC_MATCHING_LLAMA_SOURCE_DIR is required")
  endif()
  qctsm_verify_llama_source("${QCOPILOTS_TOOL_SEMANTIC_MATCHING_LLAMA_SOURCE_DIR}")
endif()
