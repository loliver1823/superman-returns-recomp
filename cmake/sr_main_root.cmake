# sr_main_root(<out-var>): the main checkout of this repository. A lane worktree has no untracked
# clones of its own (tools/XenosRecomp-clone, the FFmpeg source) and falls back to the main
# checkout's. SR_MAIN_ROOT wins; otherwise the parent of git's common directory (the main
# checkout for every worktree); otherwise this tree.
function(sr_main_root out)
  if(DEFINED ENV{SR_MAIN_ROOT})
    set(${out} "$ENV{SR_MAIN_ROOT}" PARENT_SCOPE)
    return()
  endif()
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(_main "${_root}")
  find_package(Git QUIET)
  if(GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_root}" rev-parse --path-format=absolute --git-common-dir
                    OUTPUT_VARIABLE _common OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE _rc ERROR_QUIET)
    if(_rc EQUAL 0 AND _common)
      get_filename_component(_main "${_common}/.." ABSOLUTE)
    endif()
  endif()
  set(${out} "${_main}" PARENT_SCOPE)
endfunction()
