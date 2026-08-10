#pragma once
#include <cstdint>
#include <cstddef>

#define BROKER_SOCKET_PATH "/run/secure_ipc_broker.sock"


enum class ClientRole : uint8_t {
    PRODUCER = 0,
    LISTENER = 1
};

#pragma pack(push, 1)
struct RegistrationRequest {
    char secret_token[32];   
    ClientRole role;         
    char group_name[64];     
    size_t memory_size;      // Total bytes required for ring buffer
};
#pragma pack(pop)