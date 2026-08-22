cmake_minimum_required(VERSION 3.20)
project(AutonomousVehicleApps CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Point CMake to the Framework's build folder to find the config and macros
# Replace this path with wherever your framework's build folder actually is!
set(IpcFramework_DIR "../build/framework") 
find_package(IpcFramework REQUIRED)

# Build Camera Producer
add_executable(Benchmark src/Benchmark_main.cpp)
target_link_libraries(Benchmark PRIVATE IpcClient pthread)

# Generate the camera_producer.service file right here in the build folder!
ipc_generate_secure_service(Benchmark "Producer and Consumer build" "Benchmark")

### Build Telemetry
##add_executable(telemetry_producer src/telemetry_producer.cpp)
##target_link_libraries(telemetry_producer PRIVATE IpcClient pthread)

##ipc_generate_secure_service(telemetry_producer "Vehicle Telemetry" "telemetry_app")

### Compile the Benchmark
##add_executable(benchmark_app src/Benchmark_main.cpp)
##target_link_libraries(benchmark_app PRIVATE IpcClient)
