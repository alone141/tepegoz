find_package(Threads REQUIRED)

# One program per behaviour; ../README.md describes each.
add_executable(tpz_steady Steady.cpp)
add_executable(tpz_exit ExitCode.cpp)
add_executable(tpz_crash Crash.cpp)
add_executable(tpz_stubborn Stubborn.cpp)
add_executable(tpz_heartbeat Heartbeat.cpp)
add_executable(tpz_forker Forker.cpp)
add_executable(tpz_chatty Chatty.cpp)
add_executable(tpz_env EnvReport.cpp)
add_executable(tpz_hog ResourceHog.cpp)
add_executable(tpz_watch Watch.cpp)
add_executable(tpz_request Request.cpp)

set(test_programs tpz_steady tpz_exit tpz_crash tpz_stubborn tpz_heartbeat tpz_forker tpz_chatty tpz_env tpz_hog tpz_watch tpz_request)
foreach(program IN LISTS test_programs)
    target_link_libraries(${program} PRIVATE test_service cxxopts::cxxopts)
endforeach()

target_link_libraries(tpz_hog PRIVATE Threads::Threads)
# tpz_watch reads the reports through the Process module's own struct, so the layout cannot drift from the manager's.
target_include_directories(tpz_watch PRIVATE $<TARGET_PROPERTY:Process,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(tpz_watch PRIVATE cppzmq-static)
# tpz_request sends one request, to the service registry by default, and prints the reply.
target_link_libraries(tpz_request PRIVATE cppzmq-static)

install(
    TARGETS ${test_programs}
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    RUNTIME DESTINATION ${TEPEGOZ_BIN_INSTALL_DIR}
)
