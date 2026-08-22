
macro(ipc_generate_secure_service EXECUTABLE_NAME SERVICE_DESCRIPTION SERVICE_USER)
    set(EXECUTABLE_NAME "${EXECUTABLE_NAME}")
    set(SERVICE_DESCRIPTION "${SERVICE_DESCRIPTION}")
    set(SERVICE_USER "${SERVICE_USER}")
    
    configure_file(
        "/home/karthi_keyan/IPC_hackathon/framework/sysmd/ipc-app.service.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${EXECUTABLE_NAME}.service"
        @ONLY
    )
endmacro()
