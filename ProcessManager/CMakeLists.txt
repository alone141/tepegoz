cmake_minimum_required(VERSION 3.20)
project(process_manager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_TEST "Build the GoogleTest suites and the keepalive test service" ON)

# Built into the manager. Directories end with a slash. With -DCMAKE_INSTALL_PREFIX=/usr the config is read
# from /etc/processmanager/ and the logs go under /var/log/processmanager/.
include(GNUInstallDirs)
set(PM_CONFIG_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/processmanager/" CACHE STRING "Directory of ProcessManagerConfig.yaml")
set(PM_LOG_DIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/log/processmanager/" CACHE STRING "Directory that holds ProcessManagerLog")
set(PM_SHARE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/processmanager/" CACHE STRING "Directory of the fallback ProcessManagerConfig.yaml")
set(PM_BIN_INSTALL_DIR "${CMAKE_INSTALL_BINDIR}" CACHE STRING "Install directory of the programs")

# Dependencies: libzmq with cppzmq (found here, since four directories use it), cxxopts (found in
# ProcessManagerApp/source) and GoogleTest for the tests are installed packages; fkYAML 0.4.4 is vendored in
# third_party (see third_party/README.md). Nothing is downloaded: a missing package stops the configure step
# and names what to install.
find_package(cppzmq 4.7 CONFIG QUIET)
if(NOT cppzmq_FOUND)
    message(FATAL_ERROR "cppzmq 4.7 or newer, with libzmq, was not found. Install cppzmq-dev and libzmq3-dev "
        "(Debian, Ubuntu) or cppzmq-devel and zeromq-devel (Fedora, RHEL); cppzmq 4.10 with libzmq 4.3 is tested. "
        "Add their prefix to CMAKE_PREFIX_PATH if it is not a system one.")
endif()

if(ENABLE_TEST)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(FATAL_ERROR "GoogleTest was not found. Install libgtest-dev (Debian, Ubuntu) or gtest-devel "
            "(Fedora, RHEL), GoogleTest 1.10 or newer, or configure with -DENABLE_TEST=OFF to build without the tests.")
    endif()
    enable_testing()
endif()

set(FK_YAML_USE_SINGLE_HEADER ON)
add_subdirectory(third_party/fkYAML)
# fkYAML's headers count as system headers, so its warnings stay out of ours. Set as a property because
# add_subdirectory(... SYSTEM) needs CMake 3.25, and older versions take SYSTEM for a build directory name.
get_target_property(fkyaml_include_dirs fkYAML INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(fkYAML PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${fkyaml_include_dirs}")

add_subdirectory(ConfigSection)
add_subdirectory(Logger)
add_subdirectory(NetworkManager)
add_subdirectory(GpuMonitor)
add_subdirectory(ProcessManager)
add_subdirectory(Process)
add_subdirectory(ProcessManagerApp)
