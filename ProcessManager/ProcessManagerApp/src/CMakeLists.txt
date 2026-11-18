add_executable(ProcessManagerApp)

# The paths are set in the root CMakeLists.txt; the version is the project's.
set(PROCESS_MANAGER_LOG_DIR "${TEPEGOZ_LOG_DIR}TepegozLog")
set(PROCESS_MANAGER_CONFIG_FILE "${TEPEGOZ_CONFIG_DIR}tepegoz.yaml")
target_compile_definitions(ProcessManagerApp
	PRIVATE
		PROCESS_MANAGER_LOG_DIR=\"${PROCESS_MANAGER_LOG_DIR}\"
		PROCESS_MANAGER_CONFIG_FILE=\"${PROCESS_MANAGER_CONFIG_FILE}\"
		PROCESS_MANAGER_SHARE_DIR=\"${TEPEGOZ_SHARE_DIR}\"
		PROCESS_MANAGER_VERSION_FULL=\"${PROJECT_VERSION}\"
)

target_sources(ProcessManagerApp 
	PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/../include/ProcessManagerApp.hpp
		${CMAKE_CURRENT_LIST_DIR}/../include/HelperFunctions.hpp
	PRIVATE
		${CMAKE_CURRENT_LIST_DIR}/ProcessManagerApp.cpp
		${CMAKE_CURRENT_LIST_DIR}/HelperFunctions.cpp
		${CMAKE_CURRENT_LIST_DIR}/main.cpp
)

target_include_directories(ProcessManagerApp PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(ProcessManagerApp PUBLIC ProcessManager network_manager cxxopts::cxxopts config_section config_source logger)
# status and the commands: the live table, and following what a command does. add, set, replace, remove, show and
# list: requests to the service registry.
target_link_libraries(ProcessManagerApp PRIVATE status_client command_watch registry_client)

# Installed under the product name, which install(TARGETS) cannot rename to; the build keeps the target's name.
install(
	PROGRAMS $<TARGET_FILE:ProcessManagerApp>
	PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
	DESTINATION ${TEPEGOZ_BIN_INSTALL_DIR}
	RENAME "tepegoz"
)
