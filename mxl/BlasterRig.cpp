import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "The network blaster demands a native x86_64 hardware execution loop."
#endif

extern "C" {
    int socket(int domain, int type, int protocol) noexcept;
    int connect(int sockfd, const void* addr, unsigned int addrlen) noexcept;
    long write(int fd, const void* buf, unsigned long count) noexcept;
    int close(int fd) noexcept;
    int pthread_setaffinity_np(unsigned long thread, unsigned long cpusetsize, const void* cpuset) noexcept;
    unsigned long pthread_self() noexcept;
}

constexpr int AF_INET = 2;
constexpr int SOCK_STREAM = 1;
constexpr std::uint32_t EXPECTED_MAGIC = 0x4D58524C; // 'MXRL'

struct alignas(2) LinuxSockAddrIn {
    std::uint16_t sin_family;
    std::uint16_t sin_port;
    std::uint32_t sin_addr;
    char sin_zero[8]; // Complete 8-byte array padding matching kernel specifications
};

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic{EXPECTED_MAGIC};
    std::uint32_t length{0};
    std::uint32_t flags{0};
};
#pragma pack(pop)

struct cpu_set_t {
    unsigned long __bits[1024 / (8 * sizeof(unsigned long))];
};

void pin_blaster_to_core(int core_id) noexcept {
    cpu_set_t cpuset{};
    std::memset(&cpuset, 0, sizeof(cpu_set_t));
    std::size_t word = static_cast<std::size_t>(core_id) / (8 * sizeof(unsigned long));
    std::size_t bit  = static_cast<std::size_t>(core_id) % (8 * sizeof(unsigned long));
    cpuset.__bits[word] |= (1UL << bit);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int main() {
    std::println("[+] Initializing mxreal64 C++ Bare-Metal Network Blaster Rig...");

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        std::println(std::cerr, "[-] Error: Failed to open network socket.");
        return -1;
    }

    LinuxSockAddrIn server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = 0xA05E;     // Port 24224 byte-swapped layout map
    server_addr.sin_addr = 0x0100007F; // 127.0.0.1 loopback
    std::memset(server_addr.sin_zero, 0, 8);

    if (::connect(client_fd, &server_addr, sizeof(server_addr)) < 0) {
        std::println(std::cerr, "[-] Error: Target engine refused network handshake loop.");
        ::close(client_fd);
        return -1;
    }
    std::println("[+] Socket tunnel handshake verified online.");

    std::string_view payload_msg = "ERROR Cache line collision hazard detected on hardware address boundary.";
    std::uint32_t payload_len = static_cast<std::uint32_t>(payload_msg.length());
    
    std::vector<std::byte> packet_buffer(sizeof(PacketHeader) + payload_len);
    
    PacketHeader header;
    header.length = payload_len;
    header.flags = 0;
    
    std::memcpy(packet_buffer.data(), &header, sizeof(PacketHeader));
    std::memcpy(packet_buffer.data() + sizeof(PacketHeader), payload_msg.data(), payload_len);

    pin_blaster_to_core(3);

    constexpr std::size_t TOTAL_BURST_PACKETS = 10000000; 
    std::println("[+] Pinning Blaster to CPU Core 3. Initializing whatever number TOTAL_BURST_PACKETS is packet hot-path...");

    auto start_time = std::chrono::high_resolution_clock::now();
    std::uint64_t tsc_start = ::__builtin_ia32_rdtsc();

    for (std::size_t i = 0; i < TOTAL_BURST_PACKETS; ++i) {
        long sent_bytes = ::write(client_fd, packet_buffer.data(), packet_buffer.size());
        if (sent_bytes <= 0) [[unlikely]] {
            std::println(std::cerr, "[-] Error: Socket pipeline ruptured mid-run loop. Statistics invalid.");
            break;
        }
    }

    std::uint64_t tsc_end = ::__builtin_ia32_rdtsc();
    auto end_time = std::chrono::high_resolution_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    std::uint64_t total_cycles = tsc_end - tsc_start;

    std::println("==================================================================================");
    std::println("[===] C++ Bare-Metal Blaster Run Complete! [===]"); 
    std::println("[+] Total Execution Time: {:.4f} seconds", duration);
    std::println("[+] Sustained Pipeline Velocity: {:.2f} packets/sec", TOTAL_BURST_PACKETS / duration);
    std::println("[+] Hardware Latency Footprint: {} total CPU clock cycles", total_cycles);
    std::println("[+] Average Latency Per Frame: {} clock cycles", total_cycles / TOTAL_BURST_PACKETS);

    ::close(client_fd);
    return 0;
}
