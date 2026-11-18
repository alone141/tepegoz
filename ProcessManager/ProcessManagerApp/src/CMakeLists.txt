add_executable(TepegozApp)

# The paths are set in the root CMakeLists.txt; the version is the project's.
set(TEPEGOZ_LOG_FOLDER "${TEPEGOZ_LOG_DIR}TepegozLog")
set(TEPEGOZ_CONFIG_FILE "${TEPEGOZ_CONFIG_DIR}tepegoz.yaml")
target_compile_definitions(TepegozApp
	PRIVATE
		TEPEGOZ_LOG_FOLDER=\"${TEPEGOZ_LOG_FOLDER}\"
		TEPEGOZ_CONFIG_FILE=\"${TEPEGOZ_CONFIG_FILE}\"
		TEPEGOZ_SHARE_DIR=\"${TEPEGOZ_SHARE_DIR}\"
		TEPEGOZ_VERSION_FULL=\"${PROJECT_VERSION}\"
)

target_sources(TepegozApp 
	PUBLIC
		${CMAKE_CURRENT_LIST_DIR}/../include/TepegozApp.hpp
		${CMAKE_CURRENT_LIST_DIR}/../include/HelperFunctions.hpp
	PRIVATE
		${CMAKE_CURRENT_LIST_DIR}/TepegozApp.cpp
		${CMAKE_CURRENT_LIST_DIR}/HelperFunctions.cpp
		${CMAKE_CURRENT_LIST_DIR}/main.cpp
)

target_include_directories(TepegozApp PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(TepegozApp PUBLIC Tepegoz network_manager cxxopts::cxxopts config_section config_source logger)
# status and the commands: the live table, and following what a command does. add, set, replace, remove, show and
# list: requests to the service registry.
target_link_libraries(TepegozApp PRIVATE status_client command_watch registry_client)

# Installed under the product name, which install(TARGETS) cannot rename to; the build keeps the target's name.
install(
	PROGRAMS $<TARGET_FILE:TepegozApp>
	PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
	DESTINATION ${TEPEGOZ_BIN_INSTALL_DIR}
	RENAME "tepegoz"
)
