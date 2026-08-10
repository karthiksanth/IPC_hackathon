#pragma once
#include <cstdint>

// Must be a fixed size (No std::string or std::vector!)
struct SensorData {
    uint64_t message_id;
    double temperature;
    char status[32]; 
};