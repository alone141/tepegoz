# What cmake --install sets up besides the programs: the configuration, the log directory, the systemd unit, the
# monitor GUI with its launcher and menu entry, and, last, the unit enabled so the manager starts at boot.

set(TEPEGOZ_CONFIG_SOURCE "" CACHE FILEPATH
    "tepegoz.yaml to install; empty installs a starter configuration with no services")
set(TEPEGOZ_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system" CACHE PATH
    "Directory of the installed tepegoz.service")
option(TEPEGOZ_INSTALL_GUI "Install the monitor GUI (Goz), its goz launcher and its menu entry" ON)
option(TEPEGOZ_ENABLE_SERVICE "Enable tepegoz.service after installing, when installing as root on systemd" ON)

cmake_path(ABSOLUTE_PATH TEPEGOZ_BIN_INSTALL_DIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" OUTPUT_VARIABLE installed_bin_dir)
set(INSTALLED_MANAGER "${installed_bin_dir}/tepegoz")
set(UNIT_NAME tepegoz.service)
set(GUI_INSTALL_DIR "${TEPEGOZ_SHARE_DIR}gui")

# The configuration: the share directory gets the package's copy, which the manager falls back to; the config
# directory gets it only when it has none, so an edited configuration survives a reinstall (InstallConfig.cmake).
if(TEPEGOZ_CONFIG_SOURCE)
    cmake_path(ABSOLUTE_PATH TEPEGOZ_CONFIG_SOURCE BASE_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE config_source)
    if(NOT EXISTS "${config_source}")
        message(FATAL_ERROR "TEPEGOZ_CONFIG_SOURCE names ${config_source}, which does not exist.")
    endif()
else()
    set(config_source "${CMAKE_CURRENT_BINARY_DIR}/tepegoz.yaml")
    configure_file(tepegoz.yaml.in ${config_source} @ONLY NEWLINE_STYLE UNIX)
endif()
install(FILES ${config_source} DESTINATION ${TEPEGOZ_SHARE_DIR} RENAME tepegoz.yaml)
configure_file(InstallConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/InstallConfig.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/InstallConfig.cmake)

# The folder of registered services, one <ProcessName>.yaml each, next to the configuration. Made empty when
# missing; an existing one is kept with what it holds.
install(DIRECTORY DESTINATION ${TEPEGOZ_CONFIG_DIR}services.d)

# The manager writes tepegoz_*.log files here.
install(DIRECTORY DESTINATION ${TEPEGOZ_LOG_DIR}TepegozLog)

configure_file(tepegoz.service.in ${CMAKE_CURRENT_BINARY_DIR}/${UNIT_NAME} @ONLY NEWLINE_STYLE UNIX)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${UNIT_NAME} DESTINATION ${TEPEGOZ_SYSTEMD_UNIT_DIR})

# The GUI lives next to this project; a copy of the project without it installs no GUI.
set(gui_dir "${PROJECT_SOURCE_DIR}/../gui")
if(TEPEGOZ_INSTALL_GUI AND EXISTS "${gui_dir}/goz.py")
    file(GLOB gui_sources CONFIGURE_DEPENDS "${gui_dir}/*.py")
    install(FILES ${gui_sources} DESTINATION ${GUI_INSTALL_DIR})
    configure_file(goz.in ${CMAKE_CURRENT_BINARY_DIR}/goz @ONLY NEWLINE_STYLE UNIX)
    install(
        PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/goz
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
        DESTINATION ${TEPEGOZ_BIN_INSTALL_DIR}
    )
    # Goz in the desktop's application menu, with its mark as the icon. The GUI draws the mark itself, so the
    # SVG goes only to the icon theme.
    set(INSTALLED_LAUNCHER "${installed_bin_dir}/goz")
    configure_file(goz.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/goz.desktop @ONLY NEWLINE_STYLE UNIX)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/goz.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${gui_dir}/goz.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
elseif(TEPEGOZ_INSTALL_GUI)
    message(STATUS "No gui folder next to the project: the monitor GUI is not installed")
endif()

if(TEPEGOZ_ENABLE_SERVICE)
    configure_file(EnableService.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/EnableService.cmake @ONLY)
    install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/EnableService.cmake)
endif()
