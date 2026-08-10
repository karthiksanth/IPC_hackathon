#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>

#define BROKER_SOCKET_PATH "/run/secure_ipc_broker.sock"

// Compile-time FNV-1a Hash function
constexpr uint64_t fnv1a_hash(std::string_view sv) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : sv) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Generate a unique 64-bit fingerprint for any C++ type at compile-time
template <typename T>
constexpr uint64_t get_type_fingerprint() {
#if defined(__GNUC__) || defined(__clang__)
    constexpr std::string_view name = __PRETTY_FUNCTION__;
#else
    constexpr std::string_view name = typeid(T).name();
#endif
    uint64_t h = fnv1a_hash(name);
    // Combine hash with size and alignment to detect structural shifts
    h ^= (static_cast<uint64_t>(sizeof(T)) * 0x9e3779b97f4a7c15ULL);
    h ^= (static_cast<uint64_t>(alignof(T)) * 0xbf58476d1ce4e5b9ULL);
    return h;
}
enum class ClientRole : uint8_t {
    PRODUCER = 0,
    LISTENER = 1
};

#pragma pack(push, 1)
struct RegistrationRequest {
    char secret_token[32];   
    ClientRole role;         
    char group_name[64];     
    size_t memory_size;      
    
    // NEW: Type Verification Fields
    uint64_t type_hash;      // Unique signature of the payload struct
    size_t struct_size;      // sizeof(T)
    size_t struct_align;     // alignof(T)
};
#pragma pack(pop)