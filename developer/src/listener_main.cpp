#include "ipc_client.h"
#include "my_structs.h"
#include <iostream>

int main() {
    std::cout << "Starting Listener...\n";
    
    // Connect to broker, join "sensor_topic"
    auto listener_res = IpcClient::create_listener<SensorData>("sensor_topic", 100);
    if (!listener_res) {
        std::cerr << "Failed: " << listener_res.error() << "\n";
        return 1;
    }

    IpcListener<SensorData> ring_buffer(listener_res->memory());

    std::cout << "Listening for data (0% CPU while waiting)...\n";

    while (true) {
        SensorData data{};
        
        // This blocks efficiently in the Linux Kernel using Futex until data arrives
        ring_buffer.wait_for_data(data);

        std::cout << "[Listener] Received Msg " << data.message_id 
                  << " | Temp: " << data.temperature 
                  << " | Status: " << data.status << "\n";
    }
    return 0;
}