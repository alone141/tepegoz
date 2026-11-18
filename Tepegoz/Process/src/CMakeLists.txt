add_library(Process STATIC)

target_sources(Process 
	PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/../include/ExitStatus.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/HealthPolicy.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/Pipe.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/Process.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/ProcessHandle.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/ProcessConfigParams.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/RestartMode.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/RestartPolicy.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/StdioPolicy.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/RuntimeHealth.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/StdioMode.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/StopPhase.hpp
	${CMAKE_CURRENT_LIST_DIR}/../include/ProcessGroupBuilder.hpp

	PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/HealthPolicy.cpp
	${CMAKE_CURRENT_LIST_DIR}/Pipe.cpp
	${CMAKE_CURRENT_LIST_DIR}/Process.cpp
	${CMAKE_CURRENT_LIST_DIR}/ProcessConfigParams.cpp
	${CMAKE_CURRENT_LIST_DIR}/RestartPolicy.cpp
	${CMAKE_CURRENT_LIST_DIR}/StdioPolicy.cpp
	${CMAKE_CURRENT_LIST_DIR}/ProcessGroupBuilder.cpp

)

target_include_directories(Process PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../include")
target_link_libraries(Process PUBLIC config_section logger)
