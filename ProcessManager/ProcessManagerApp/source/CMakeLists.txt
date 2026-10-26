add_executable(ProcessManagerApp)

# The paths are set in the root CMakeLists.txt; the version is the project's.
set(PROCESS_MANAGER_LOG_DIR "${PM_LOG_DIR}ProcessManagerLog")
set(PROCESS_MANAGER_CONFIG_FILE "${PM_CONFIG_DIR}ProcessManagerConfig.yaml")
target_compile_definitions(ProcessManagerApp
	PRIVATE
		PROCESS_MANAGER_LOG_DIR=\"${PROCESS_MANAGER_LOG_DIR}\"
		PROCESS_MANAGER_CONFIG_FILE=\"${PROCESS_MANAGER_CONFIG_FILE}\"
		PROCESS_MANAGER_SHARE_DIR=\"${PM_SHARE_DIR}\"
		PROCESSMANAGER_VERSION_FULL=\"${PROJECT_VERSION}\"
)

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
target_link_libraries(ProcessManagerApp PUBLIC ProcessManager network_manager cxxopts::cxxopts config_section logger)

# Installed under the product name, which install(TARGETS) cannot rename to; the build keeps the target's name.
install(
	PROGRAMS $<TARGET_FILE:ProcessManagerApp>
	PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
	DESTINATION ${PM_BIN_INSTALL_DIR}
	RENAME "iltaren-ProcessManager"
)
