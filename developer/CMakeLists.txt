# Compile the Benchmark
add_executable(benchmark_app src/Benchmark_main.cpp)
target_link_libraries(benchmark_app PRIVATE IpcClient)
##cmake_minimum_required(VERSION 3.20)
##project(AutonomousVehicleApps CXX)
##
##set(CMAKE_CXX_STANDARD 23)
##set(CMAKE_CXX_STANDARD_REQUIRED ON)
##
### 1. Find the framework installed on the system
##find_package(IpcFramework REQUIRED)
##
### ==========================================================
### PAIR 1: Camera Feed (1 Producer -> 1 Listener)
### ==========================================================
##
### Camera Producer
##add_executable(camera_producer src/camera_producer.cpp)
##target_link_libraries(camera_producer PRIVATE IpcFramework::Client)
##install(TARGETS camera_producer DESTINATION bin)
##
### Use the framework's macro to generate a secure systemd file
##ipc_generate_secure_service(camera_producer "High-Res Camera Producer" "sensor_app")
##
### Camera AI Listener
##add_executable(ai_listener src/ai_listener.cpp)
##target_link_libraries(ai_listener PRIVATE IpcFramework::Client)
##install(TARGETS ai_listener DESTINATION bin)
##ipc_generate_secure_service(ai_listener "AI Vision Processor" "ai_app")
##
##
### ==========================================================
### PAIR 2: Telemetry (1 Producer -> Multiple Listeners)
### ==========================================================
##
### CAN Bus Producer
##add_executable(telemetry_producer src/telemetry_producer.cpp)
##target_link_libraries(telemetry_producer PRIVATE IpcFramework::Client)
##install(TARGETS telemetry_producer DESTINATION bin)
##ipc_generate_secure_service(telemetry_producer "Vehicle Telemetry" "sensor_app")