add_executable(ProcessManagerApp)

set(PROCESS_MANAGER_LOG_DIR "${PM_LOG_DIR}ProcessManagerLog")
add_compile_definitions(PROCESS_MANAGER_LOG_DIR=\"${PROCESS_MANAGER_LOG_DIR}\")
set(PROCESS_MANAGER_CONFIG_FILE "${PM_CONFIG_DIR}ProcessManagerConfig.yaml")
add_compile_definitions(PROCESS_MANAGER_CONFIG_FILE=\"${PROCESS_MANAGER_CONFIG_FILE}\")

target_sources(ProcessManagerApp 
	PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/include/ProcessManagerApp.h
		${CMAKE_CURRENT_LIST_DIR}/include/HelperFunctions.h
	PRIVATE
		${CMAKE_CURRENT_LIST_DIR}/src/ProcessManagerApp.cpp
		${CMAKE_CURRENT_LIST_DIR}/src/HelperFunctions.cpp
		${CMAKE_CURRENT_LIST_DIR}/src/main.cpp
)

target_include_directories(ProcessManagerApp PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(ProcessManagerApp PUBLIC ProcessManager NetworkManager cxxopts config_section logger)

install(
	TARGETS ProcessManagerApp 
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
	RUNTIME DESTINATION ${PM_BIN_INSTALL_DIR}
	RENAME "iltaren-ProcessManager"
)
