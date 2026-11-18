add_executable(keepalive "keepalive.cpp")
target_link_libraries(keepalive PUBLIC cppzmq-static)

install(
	TARGETS keepalive 
	PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
	RUNTIME DESTINATION ${TEPEGOZ_BIN_INSTALL_DIR}
)