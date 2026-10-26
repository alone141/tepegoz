cmake_minimum_required(VERSION 3.20)
project(process_manager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TEST "Build the GoogleTest suites, the keepalive test service and the TestServices programs" ON)

# Built into the manager. Directories end with a slash. With -DCMAKE_INSTALL_PREFIX=/usr the config is read
# from /etc/processmanager/ and the logs go under /var/log/processmanager/.
include(GNUInstallDirs)
set(PM_CONFIG_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/processmanager/" CACHE STRING "Directory of ProcessManagerConfig.yaml")
set(PM_LOG_DIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/log/processmanager/" CACHE STRING "Directory that holds ProcessManagerLog")
set(PM_SHARE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/processmanager/" CACHE STRING "Directory of the fallback ProcessManagerConfig.yaml")
set(PM_BIN_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}" CACHE STRING "Install directory of the programs")

# Dependencies: libzmq 4.3.5, cppzmq 4.10.0, cxxopts 3.3.1, fkYAML 0.4.4 and, for the tests, GoogleTest 1.18.0 are
# vendored in third_party and built here (see third_party/README.md): the build needs no installed package and
# downloads nothing. The vendored directories other than fkYAML are EXCLUDE_FROM_ALL: only what our targets link is
# built, and nothing of theirs is installed.

# libzmq builds only its static library, without tests, tools, docs or optional system libraries, so the programs
# need no libzmq at run time. CMP0077 lets its options take these variables. CMake 4 refuses its
# cmake_minimum_required(VERSION 2.8.12) unless CMAKE_POLICY_VERSION_MINIMUM raises it; CMake 3.27 to 3.31 only warn
# that it is deprecated.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
set(BUILD_SHARED OFF)
set(BUILD_STATIC ON)
set(BUILD_TESTS OFF)
set(WITH_PERF_TOOL OFF)
set(WITH_DOCS OFF)
set(WITH_LIBBSD OFF)
set(ENABLE_DRAFTS OFF)
set(ENABLE_CPACK OFF)
set(build_type_before_libzmq "${CMAKE_BUILD_TYPE}")
add_subdirectory(third_party/libzmq EXCLUDE_FROM_ALL)
# libzmq makes Release the default build type of the whole build; the project keeps CMake's, which is none.
if(NOT build_type_before_libzmq AND NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY VALUE "")
endif()
unset(CMAKE_POLICY_VERSION_MINIMUM)

# Its cppzmq-static target is the two headers with libzmq-static. Its tests would download Catch2.
set(CPPZMQ_BUILD_TESTS OFF)
add_subdirectory(third_party/cppzmq EXCLUDE_FROM_ALL)

# As a subproject, cxxopts builds no tests or examples and installs nothing; the manager links cxxopts::cxxopts.
add_subdirectory(third_party/cxxopts EXCLUDE_FROM_ALL)

if(ENABLE_TEST)
    # Only gtest and gtest_main, which the suites link as GTest::gtest_main.
    set(BUILD_GMOCK OFF)
    set(INSTALL_GTEST OFF)
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    enable_testing()
endif()

set(FK_YAML_USE_SINGLE_HEADER ON)
add_subdirectory(third_party/fkYAML)

# The vendored headers count as system headers, so their warnings stay out of ours. Set as a property because
# add_subdirectory(... SYSTEM) needs CMake 3.25, and older versions take SYSTEM for a build directory name.
foreach(vendored IN ITEMS libzmq-static cppzmq-static cxxopts fkYAML)
    get_target_property(vendored_include_dirs ${vendored} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${vendored} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${vendored_include_dirs}")
endforeach()

add_subdirectory(ConfigSection)
add_subdirectory(Logger)
add_subdirectory(NetworkManager)
add_subdirectory(GpuMonitor)
add_subdirectory(JournalServer)
add_subdirectory(ProcessManager)
add_subdirectory(Process)
add_subdirectory(ProcessManagerApp)

if(ENABLE_TEST)
    add_subdirectory(TestServices)
endif()

# Last, since its install step enables the service once everything else is installed.
add_subdirectory(packaging)
