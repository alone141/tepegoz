add_library(Tepegoz STATIC)

target_sources(Tepegoz 
	PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/../include/ITepegoz.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/Tepegoz.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/TepegozParams.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/TepegozCommands.hpp


	PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/Tepegoz.cpp
	${CMAKE_CURRENT_LIST_DIR}/TepegozRegistry.cpp


)

target_include_directories(Tepegoz PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
# GetVersionInfo answers with the project's version.
target_compile_definitions(Tepegoz
	PRIVATE
		TEPEGOZ_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
		TEPEGOZ_VERSION_MINOR=${PROJECT_VERSION_MINOR}
		TEPEGOZ_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_link_libraries(Tepegoz PUBLIC Process cppzmq-static config_section config_source logger service_registry)
target_link_libraries(Tepegoz PRIVATE gpu_monitor journal_server reload_plan)
