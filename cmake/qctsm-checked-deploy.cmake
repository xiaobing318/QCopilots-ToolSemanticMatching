include_guard(GLOBAL)

function(qctsm_sign_macos_bundle bundle python helper report identity entitlements_map)
    set(arguments "${python}" -B "${helper}" --bundle "${bundle}"
        "--identity=${identity}" --report "${report}")
    if(NOT entitlements_map STREQUAL "")
        list(APPEND arguments --entitlements-map "${entitlements_map}")
    endif()
    execute_process(COMMAND ${arguments} RESULT_VARIABLE signing_result
        OUTPUT_VARIABLE signing_output ERROR_VARIABLE signing_errors)
    message("${signing_output}")
    if(NOT signing_result STREQUAL "0")
        message(FATAL_ERROR "Final bundle signing failed: ${signing_errors}")
    endif()
endfunction()

function(qctsm_verify_macos_links bundle python helper report)
    execute_process(COMMAND "${python}" -B "${helper}" --bundle "${bundle}" --report "${report}"
        RESULT_VARIABLE inspect_result OUTPUT_VARIABLE inspect_output ERROR_VARIABLE inspect_errors)
    message("${inspect_output}")
    if(NOT inspect_result STREQUAL "0")
        message(FATAL_ERROR "Deployed bundle has invalid runtime dependency paths: ${inspect_errors}")
    endif()
endfunction()

function(qctsm_verify_bundle_signature bundle)
    find_program(QCOPILOTS_TOOL_SEMANTIC_MATCHING_CODESIGN codesign REQUIRED)
    execute_process(COMMAND "${QCOPILOTS_TOOL_SEMANTIC_MATCHING_CODESIGN}" --verify --deep --strict "${bundle}"
        RESULT_VARIABLE signature_result OUTPUT_VARIABLE signature_output ERROR_VARIABLE signature_errors)
    if(NOT signature_result STREQUAL "0")
        message(FATAL_ERROR "Deployed bundle signature is invalid: ${signature_output}${signature_errors}")
    endif()
endfunction()

function(qctsm_checked_deploy deploy_script)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}"
            "-DCMAKE_INSTALL_CONFIG_NAME=${CMAKE_INSTALL_CONFIG_NAME}"
            -P "${deploy_script}"
        RESULT_VARIABLE deploy_result
        OUTPUT_VARIABLE deploy_output
        ERROR_VARIABLE deploy_errors)
    if(NOT deploy_output STREQUAL "")
        message("${deploy_output}")
    endif()
    if(NOT deploy_errors STREQUAL "")
        message("${deploy_errors}")
    endif()
    if(NOT deploy_result STREQUAL "0" OR
            "${deploy_output}\n${deploy_errors}" MATCHES "(^|[\r\n])[ \t]*ERROR:")
        message(FATAL_ERROR "Qt deployment failed or reported an incomplete bundle")
    endif()
endfunction()
