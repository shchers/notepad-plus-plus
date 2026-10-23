# CPACK_PRE_BUILD_SCRIPTS hook: normalise permissions of the staged tree
# independent of the umask cpack ran under. Directories become 0755, files
# 0644, and executables keep their execute bits (0755).
execute_process(COMMAND chmod -R u+rwX,go+rX,go-w "${CPACK_TEMPORARY_DIRECTORY}"
                RESULT_VARIABLE _rc)
if(_rc)
  message(FATAL_ERROR "chmod of ${CPACK_TEMPORARY_DIRECTORY} failed: ${_rc}")
endif()
