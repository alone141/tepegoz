find_package(Threads REQUIRED)

# One program per behaviour; ../README.md describes each.
add_executable(pmt_steady Steady.cpp)
add_executable(pmt_exit ExitCode.cpp)
add_executable(pmt_crash Crash.cpp)
add_executable(pmt_stubborn Stubborn.cpp)
add_executable(pmt_heartbeat Heartbeat.cpp)
add_executable(pmt_forker Forker.cpp)
add_executable(pmt_chatty Chatty.cpp)
add_executable(pmt_env EnvReport.cpp)
add_executable(pmt_hog ResourceHog.cpp)
add_executable(pmt_watch Watch.cpp)
add_executable(pmt_request Request.cpp)

set(test_programs pmt_steady pmt_exit pmt_crash pmt_stubborn pmt_heartbeat pmt_forker pmt_chatty pmt_env pmt_hog pmt_watch pmt_request)
foreach(program IN LISTS test_programs)
    target_link_libraries(${program} PRIVATE test_service cxxopts::cxxopts)
endforeach()

target_link_libraries(pmt_hog PRIVATE Threads::Threads)
# pmt_watch reads the reports through the Process module's own struct, so the layout cannot drift from the manager's.
target_include_directories(pmt_watch PRIVATE $<TARGET_PROPERTY:Process,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(pmt_watch PRIVATE cppzmq-static)
# pmt_request sends one request, to the service registry by default, and prints the reply.
target_link_libraries(pmt_request PRIVATE cppzmq-static)

install(
    TARGETS ${test_programs}
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    RUNTIME DESTINATION ${PM_BIN_INSTALL_DIR}
)
