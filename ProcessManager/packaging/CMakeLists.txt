# What cmake --install sets up besides the programs: the configuration, the log directory, the systemd unit, the
# monitor GUI with its launcher, and, last, the unit enabled so the manager starts at boot.

set(PM_CONFIG_SOURCE "" CACHE FILEPATH
    "ProcessManagerConfig.yaml to install; empty installs a starter configuration with no services")
set(PM_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system" CACHE PATH
    "Directory of the installed processmanager.service")
option(PM_INSTALL_GUI "Install the monitor GUI (ProcessManagerGui) and its process-monitor launcher" ON)
option(PM_ENABLE_SERVICE "Enable processmanager.service after installing, when installing as root on systemd" ON)

cmake_path(ABSOLUTE_PATH PM_BIN_INSTALL_DIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" OUTPUT_VARIABLE installed_bin_dir)
set(INSTALLED_MANAGER "${installed_bin_dir}/iltaren-ProcessManager")
set(UNIT_NAME processmanager.service)
set(GUI_INSTALL_DIR "${PM_SHARE_DIR}gui")

# The configuration: the share directory gets the package's copy, which the manager falls back to; the config
# directory gets it only when it has none, so an edited configuration survives a reinstall (InstallConfig.cmake).
if(PM_CONFIG_SOURCE)
    cmake_path(ABSOLUTE_PATH PM_CONFIG_SOURCE BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE config_source)
    if(NOT EXISTS "${config_source}")
        message(FATAL_ERROR "PM_CONFIG_SOURCE names ${config_source}, which does not exist.")
    endif()
else()
    set(config_source "${CMAKE_CURRENT_BINARY_DIR}/ProcessManagerConfig.yaml")
    configure_file(ProcessManagerConfig.yaml.in ${config_source} @ONLY NEWLINE_STYLE UNIX)
endif()
install(FILES ${config_source} DESTINATION ${PM_SHARE_DIR} RENAME ProcessManagerConfig.yaml)
configure_file(InstallConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/InstallConfig.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/InstallConfig.cmake)

# The manager writes PMLog_*.log files here.
install(DIRECTORY DESTINATION ${PM_LOG_DIR}ProcessManagerLog)

configure_file(processmanager.service.in ${CMAKE_CURRENT_BINARY_DIR}/${UNIT_NAME} @ONLY NEWLINE_STYLE UNIX)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${UNIT_NAME} DESTINATION ${PM_SYSTEMD_UNIT_DIR})

# The GUI lives next to this project; a copy of the project without it installs no GUI.
set(gui_dir "${PROJECT_SOURCE_DIR}/../ProcessManagerGui")
if(PM_INSTALL_GUI AND EXISTS "${gui_dir}/process_monitor_gui.py")
    file(GLOB gui_sources CONFIGURE_DEPENDS "${gui_dir}/*.py")
    install(FILES ${gui_sources} DESTINATION ${GUI_INSTALL_DIR})
    configure_file(process-monitor.in ${CMAKE_CURRENT_BINARY_DIR}/process-monitor @ONLY NEWLINE_STYLE UNIX)
    install(
        PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/process-monitor
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        DESTINATION ${PM_BIN_INSTALL_DIR}
    )
elseif(PM_INSTALL_GUI)
    message(STATUS "No ProcessManagerGui next to the project: the monitor GUI is not installed")
endif()

if(PM_ENABLE_SERVICE)
    configure_file(EnableService.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/EnableService.cmake @ONLY)
    install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/EnableService.cmake)
endif()
