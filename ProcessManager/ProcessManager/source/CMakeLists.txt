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
target_link_libraries(ProcessManager PUBLIC Process cppzmq config_section logger)
target_link_libraries(ProcessManager PRIVATE gpu_monitor)
