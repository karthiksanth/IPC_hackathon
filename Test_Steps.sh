# 1. Setup the groups (Run this once)
sudo groupadd ipc_group
sudo useradd --system -g ipc_group Benchmark
#sudo useradd --system -g ipc_group telemetry_app

# 2. Copy the token generator to the OS (It has no build folder)
sudo cp ../framework/sysmd/ipc-token-gen.service /etc/systemd/system/

# 3. Tell systemd to read your services directly from your build folders!
sudo systemctl link ./framework/ipc_broker.service
sudo systemctl link ./developer/Benchmark.service

# 4. Reload systemd to recognize the new links
sudo systemctl daemon-reload

# 5. Run the Demo!
# Start the broker with the DEV MODE environment variable pointing to your workspace
sudo systemctl set-environment IPC_TRUSTED_DIR="./developer/"
sudo systemctl start ipc_broker

sudo systemctl start Benchmark

#sudo bash -c "source /run/ipc_secrets && export IPC_TRUSTED_DIR=$(pwd)/ && ./ipc_broker"