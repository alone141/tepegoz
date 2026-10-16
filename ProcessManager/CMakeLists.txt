
#project("ProcessManager" DESCRIPTION "ProcessManager" LANGUAGES C CXX)

# Dependencies: NetworkManager, libzmq with cppzmq, and cxxopts, from the build that adds this directory,
# and fkYAML 0.4.4, vendored in third_party (see third_party/README.md). SYSTEM keeps fkYAML's warnings out of ours.
set(FK_YAML_USE_SINGLE_HEADER ON)
add_subdirectory(third_party/fkYAML SYSTEM)

add_subdirectory(ConfigSection)
add_subdirectory(Logger)
add_subdirectory(GpuMonitor)
add_subdirectory(ProcessManager)
add_subdirectory(Process)
add_subdirectory(ProcessManagerApp)


