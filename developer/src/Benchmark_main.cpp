#include "ipc_client.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <iomanip>

using namespace std::chrono;

// --- Benchmark Configurations ---
constexpr int WARMUP_ITERS = 10'000;
constexpr int TEST_ITERS = 1'000'000;
constexpr int VARIABLE_PAYLOAD_SIZE = 1024; // 1 KB variable payload

// Struct for Fixed Benchmark
struct FixedBenchData {
    uint64_t sequence_id;
};

// Struct for Variable Benchmark (Metadata)
struct VarBenchMeta {
    uint64_t sequence_id;
};

// ============================================================================
// 1. FIXED RING BUFFER BENCHMARK
// ============================================================================
void run_fixed_benchmark() {
    // Setup Client Connections
    auto pinger_tx_res = IpcClient::create_fixed_producer<FixedBenchData>("fixed_ping", 1024);
    auto pinger_rx_res = IpcClient::create_fixed_listener<FixedBenchData>("fixed_pong", 1024);
    
    IpcProducer<FixedBenchData> tx(pinger_tx_res->memory(), 1024);
    IpcListener<FixedBenchData> rx(pinger_rx_res->memory());

    FixedBenchData msg{};

    // Warmup
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        msg.sequence_id = i;
        tx.publish(msg);
        rx.wait_for_data(msg);
    }

    // Actual Test
    auto start = high_resolution_clock::now();
    for (int i = 0; i < TEST_ITERS; ++i) {
        msg.sequence_id = i;
        tx.publish(msg);
        rx.wait_for_data(msg);
    }
    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    
    // Divide by 2 because this is a Round-Trip (Ping + Pong)
    double one_way_latency_ns = static_cast<double>(duration_ns) / TEST_ITERS / 2.0;
    double throughput_msg_sec = (TEST_ITERS * 1e9) / duration_ns * 2.0; // Total messages / time

    std::cout << "| Fixed Ring Buffer    | " 
              << std::setw(10) << one_way_latency_ns << " ns | "
              << std::setw(15) << (int)throughput_msg_sec << " msg/sec |\n";
}

// ============================================================================
// 2. VARIABLE RING BUFFER BENCHMARK
// ============================================================================
void run_variable_benchmark() {
    auto pinger_tx_res = IpcClient::create_variable_producer<VarBenchMeta>("var_ping", 1024, 50 * 1024 * 1024);
    auto pinger_rx_res = IpcClient::create_variable_listener<VarBenchMeta>("var_pong", 1024, 50 * 1024 * 1024);
    
    VariableProducer<VarBenchMeta> tx(pinger_tx_res->memory(), 1024, 50 * 1024 * 1024);
    VariableListener<VarBenchMeta> rx(pinger_rx_res->memory());

    VarBenchMeta meta{};
    std::vector<std::byte> payload(VARIABLE_PAYLOAD_SIZE, std::byte{0xFF}); 

    // Warmup
    for (int i = 0; i < WARMUP_ITERS; ++i) {
        meta.sequence_id = i;
        tx.publish(meta, payload);
        rx.wait_and_read([&](const VarBenchMeta& m, std::span<const std::byte> d) {
            meta.sequence_id = m.sequence_id; 
        });
    }

    // Actual Test
    auto start = high_resolution_clock::now();
    for (int i = 0; i < TEST_ITERS; ++i) {
        meta.sequence_id = i;
        tx.publish(meta, payload);
        rx.wait_and_read([&](const VarBenchMeta& m, std::span<const std::byte> d) {
            meta.sequence_id = m.sequence_id; 
        });
    }
    auto end = high_resolution_clock::now();

    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    
    double one_way_latency_ns = static_cast<double>(duration_ns) / TEST_ITERS / 2.0;
    double throughput_msg_sec = (TEST_ITERS * 1e9) / duration_ns * 2.0; 

    std::cout << "| Variable Ring Buffer | " 
              << std::setw(10) << one_way_latency_ns << " ns | "
              << std::setw(15) << (int)throughput_msg_sec << " msg/sec |\n";
}

// ============================================================================
// MAIN EXECUTION (Forking Logic)
// ============================================================================
int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // --- CHILD PROCESS (The "Ponger") ---
        // Setup Fixed Ponger
        auto ponger_rx_fixed = IpcClient::create_fixed_listener<FixedBenchData>("fixed_ping", 1024);
        auto ponger_tx_fixed = IpcClient::create_fixed_producer<FixedBenchData>("fixed_pong", 1024);
        IpcListener<FixedBenchData> rx_fixed(ponger_rx_fixed->memory());
        IpcProducer<FixedBenchData> tx_fixed(ponger_tx_fixed->memory(), 1024);

        FixedBenchData f_msg{};
        for (int i = 0; i < WARMUP_ITERS + TEST_ITERS; ++i) {
            rx_fixed.wait_for_data(f_msg);
            tx_fixed.publish(f_msg);
        }

        // Setup Variable Ponger
        auto ponger_rx_var = IpcClient::create_variable_listener<VarBenchMeta>("var_ping", 1024, 50 * 1024 * 1024);
        auto ponger_tx_var = IpcClient::create_variable_producer<VarBenchMeta>("var_pong", 1024, 50 * 1024 * 1024);
        VariableListener<VarBenchMeta> rx_var(ponger_rx_var->memory());
        VariableProducer<VarBenchMeta> tx_var(ponger_tx_var->memory(), 1024, 50 * 1024 * 1024);

        for (int i = 0; i < WARMUP_ITERS + TEST_ITERS; ++i) {
            rx_var.wait_and_read([&](const VarBenchMeta& m, std::span<const std::byte> d) {
                tx_var.publish(m, d);
            });
        }
        
        exit(0); // Child exits cleanly
    } else {
        // --- PARENT PROCESS (The "Pinger") ---
        // Give the child 100ms to register with the broker before we start pinging
        std::this_thread::sleep_for(milliseconds(100));

        std::cout << "\n=========================================================\n";
        std::cout << "      ZERO-COPY IPC BENCHMARK (1,000,000 Messages)       \n";
        std::cout << "=========================================================\n";
        std::cout << "| Architecture         | Latency (1-way)| Throughput      |\n";
        std::cout << "|----------------------|----------------|-----------------|\n";
        
        run_fixed_benchmark();
        run_variable_benchmark();
        
        std::cout << "=========================================================\n\n";

        waitpid(pid, nullptr, 0); // Wait for child to finish
    }

    return 0;
}