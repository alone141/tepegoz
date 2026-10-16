
#project("ProcessManager" DESCRIPTION "ProcessManager" LANGUAGES C CXX)

# Dependencies: NetworkManager, libzmq with cppzmq, and cxxopts, from the build that adds this directory,
# and fkYAML 0.4.4, vendored in third_party (see third_party/README.md).
set(FK_YAML_USE_SINGLE_HEADER ON)
add_subdirectory(third_party/fkYAML)
# fkYAML's headers count as system headers, so its warnings stay out of ours. Set as a property because
# add_subdirectory(... SYSTEM) needs CMake 3.25, and older versions take SYSTEM for a build directory name.
get_target_property(fkyaml_include_dirs fkYAML INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(fkYAML PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${fkyaml_include_dirs}")

add_subdirectory(ConfigSection)
add_subdirectory(Logger)
add_subdirectory(GpuMonitor)
add_subdirectory(ProcessManager)
add_subdirectory(Process)
add_subdirectory(ProcessManagerApp)


