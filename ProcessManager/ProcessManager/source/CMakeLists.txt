add_library(ProcessManager STATIC)

target_sources(ProcessManager 
	PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/include/IProcessManager.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessManager.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessManagerParams.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessManagerCommands.h


	PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/src/ProcessManager.cpp


)

target_include_directories(ProcessManager PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
# GetVersionInfo answers with the project's version.
target_compile_definitions(ProcessManager
	PRIVATE
		PROCESSMANAGER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
		PROCESSMANAGER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
		PROCESSMANAGER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
target_link_libraries(ProcessManager PUBLIC Process cppzmq-static config_section logger)
target_link_libraries(ProcessManager PRIVATE gpu_monitor journal_server)
