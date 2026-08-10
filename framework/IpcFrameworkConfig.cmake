# This file is loaded by the Developer's CMakeLists.txt

# Tell CMake where the headers are installed
add_library(IpcFramework::Client INTERFACE IMPORTED)
set_target_properties(IpcFramework::Client PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_INSTALL_PREFIX}/include/IpcFramework"
)

# Provide a macro for developers to generate their own systemd files
macro(ipc_generate_secure_service TARGET_NAME DESCRIPTION SYSTEM_USER)
    set(EXECUTABLE_NAME ${TARGET_NAME})
    set(SERVICE_DESCRIPTION "${DESCRIPTION}")
    set(SERVICE_USER ${SYSTEM_USER})
    
    # Locate the template installed by the framework
    set(TEMPLATE_FILE "${CMAKE_CURRENT_LIST_DIR}/systemd/ipc_app.service.in")
    
    configure_file(
        ${TEMPLATE_FILE}
        ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.service
        @ONLY
    )
    
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.service
            DESTINATION /lib/systemd/system)
endmacro()