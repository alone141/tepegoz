add_library(Process STATIC)

target_sources(Process 
	PUBLIC
	${CMAKE_CURRENT_LIST_DIR}/include/ExitStatusStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/HealthPolicyStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/ConfigHelper.h
	${CMAKE_CURRENT_LIST_DIR}/include/PipeStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/Process.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessHandle.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessConfigParamsStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/RestartModeEnum.h
	${CMAKE_CURRENT_LIST_DIR}/include/RestartPolicyStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/StdioConfigStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/RuntimeHealthStruct.h
	${CMAKE_CURRENT_LIST_DIR}/include/StdioModeEnum.h
	${CMAKE_CURRENT_LIST_DIR}/include/StopPhaseEnum.h
	${CMAKE_CURRENT_LIST_DIR}/include/ProcessGroupBuilder.h

	PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/src/HealthPolicyStruct.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/PipeStruct.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/ConfigHelper.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/Process.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/ProcessParamsStruct.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/RestartPolicyStruct.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/StdioConfigStruct.cpp
	${CMAKE_CURRENT_LIST_DIR}/src/ProcessGroupBuilder.cpp

)

target_include_directories(Process PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(Process PUBLIC LegacyLibA)
