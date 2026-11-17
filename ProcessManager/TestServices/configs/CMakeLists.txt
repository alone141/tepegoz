# A scenario config names the test programs by absolute path, so each is written twice: for the build tree, with
# the programs where they are built, and for the install tree, with the programs where they are installed.
set(scenarios basic restarts stopping health process_tree stdio environment resources everything registered)

cmake_path(ABSOLUTE_PATH PM_BIN_INSTALL_DIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" OUTPUT_VARIABLE installed_bin_dir)

foreach(scenario IN LISTS scenarios)
    # The build tree's folder is only known at generate time, so file(GENERATE) fills in the generator expression.
    set(PMT_BIN_DIR "$<TARGET_FILE_DIR:pmt_steady>")
    configure_file(${scenario}.yaml.in ${CMAKE_CURRENT_BINARY_DIR}/unresolved/${scenario}.yaml @ONLY NEWLINE_STYLE UNIX)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${scenario}.yaml INPUT ${CMAKE_CURRENT_BINARY_DIR}/unresolved/${scenario}.yaml)

    set(PMT_BIN_DIR "${installed_bin_dir}")
    configure_file(${scenario}.yaml.in ${CMAKE_CURRENT_BINARY_DIR}/install/${scenario}.yaml @ONLY NEWLINE_STYLE UNIX)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/install/${scenario}.yaml DESTINATION ${PM_SHARE_DIR}test-setup)
endforeach()
