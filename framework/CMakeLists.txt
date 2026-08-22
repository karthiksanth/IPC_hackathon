cmake_minimum_required(VERSION 3.20)
project(IpcFramework CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(EXECUTABLE_NAME "ipc_broker")
set(SERVICE_USER "ipc_broker")
# 1. Build Framework
add_library(IpcClient INTERFACE)
target_include_directories(IpcClient INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(ipc_broker src/ipc_broker.cpp)
target_link_libraries(ipc_broker PRIVATE IpcClient pthread)

# 2. Export the Macro for the Developer Apps
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/IpcFrameworkConfig.cmake "
macro(ipc_generate_secure_service EXECUTABLE_NAME SERVICE_DESCRIPTION SERVICE_USER)
    set(EXECUTABLE_NAME \"\${EXECUTABLE_NAME}\")
    set(SERVICE_DESCRIPTION \"\${SERVICE_DESCRIPTION}\")
    set(SERVICE_USER \"\${SERVICE_USER}\")
    
    configure_file(
        \"${CMAKE_CURRENT_SOURCE_DIR}/sysmd/ipc-app.service.in\"
        \"\${CMAKE_CURRENT_BINARY_DIR}/\${EXECUTABLE_NAME}.service\"
        @ONLY
    )
endmacro()
")

# 3. Generate the Broker's systemd file (pointing to this build folder)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/sysmd/ipc-app.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/ipc_broker.service
    @ONLY
)
### Create the header-only framework library
##add_library(IpcClient INTERFACE)
##target_include_directories(IpcClient INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
##
### Compile the Broker
##add_executable(ipc_broker src/ipc_broker.cpp)
##target_link_libraries(ipc_broker PRIVATE IpcClient)
