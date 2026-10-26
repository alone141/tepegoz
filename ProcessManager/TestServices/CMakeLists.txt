# Test programs with planned behaviours, the scenario configs that run them under the manager, and the driver that
# checks the manager's behaviour with them. Added by the root CMakeLists.txt only with ENABLE_TEST.
add_subdirectory(src)
add_subdirectory(apps)
add_subdirectory(configs)
add_subdirectory(tests)

install(FILES README.md scripts/run_scenarios.py DESTINATION ${PM_SHARE_DIR}test-setup)
