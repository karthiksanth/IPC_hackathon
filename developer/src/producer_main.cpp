#include "ipc_client.h"
#include "my_struct.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <ipc_ringbuffer.h>

int main() {
    std::cout << "Starting Producer...\n";
    
    // Connect to broker, request 100 slots for "sensor_topic"
    auto producer_res = IpcClient::create_producer<SensorData>("sensor_topic", 100);
    if (!producer_res) {
        std::cerr << "Failed: " << producer_res.error() << "\n";
        return 1;
    }

    // Wrap the raw memory in our Lock-Free Producer class
    IpcProducer<SensorData> ring_buffer(producer_res->memory(), 100);

    uint64_t counter = 0;
    while (true) {
        SensorData data{};
        data.message_id = ++counter;
        data.temperature = 22.5 + (counter % 10);
        std::strncpy(data.status, "SYSTEM_OK", sizeof(data.status));

        std::cout << "[Producer] Sending Msg " << data.message_id 
                  << " | Temp: " << data.temperature << "\n";

        // Write directly to shared RAM and instantly wake listeners via Futex
        ring_buffer.publish(data);

        // Sleep for 1 second to make the demo easy to watch
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}