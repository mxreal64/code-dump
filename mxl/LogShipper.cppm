
export module LogShipper;

import std;

#if !defined(__x86_64__) && !defined(_M_X64)
#error "This shipping engine requires an x86_64 execution pipeline."
#endif

extern "C" {
    int socket(int domain, int type, int protocol) noexcept;
    int connect(int sockfd, const void* addr, unsigned int addrlen) noexcept;
    long writev(int fd, const void* iov, int iovcnt) noexcept;
    int close(int fd) noexcept;
}

constexpr int AF_INET = 2;
constexpr int SOCK_STREAM = 1;

struct iovec {
    void* iov_base;       
    std::size_t iov_len;  
};

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic{0x4D58524C}; // 'MXRL'
    std::uint32_t length{0};
    std::uint32_t flags{0};
};
#pragma pack(pop)

struct alignas(2) LinuxSockAddrIn {
    std::uint16_t sin_family;
    std::uint16_t sin_port;
    std::uint32_t sin_addr;
    char sin_zero[8];
};

export class BareMetalShipper {
private:
    int socket_fd_{-1};
    LinuxSockAddrIn server_address_;

public:
    BareMetalShipper() noexcept {
        server_address_.sin_family = AF_INET;
        server_address_.sin_port = 0;
        server_address_.sin_addr = 0;
        std::memset(server_address_.sin_zero, 0, sizeof(server_address_.sin_zero));
    }
    
    ~BareMetalShipper() {
        if (socket_fd_ >= 0) ::close(socket_fd_);
    }

    BareMetalShipper(const BareMetalShipper&) = delete;
    BareMetalShipper& operator=(const BareMetalShipper&) = delete;

    bool connect_to_aggregator(std::uint32_t ip_be32, std::uint16_t port_be16) noexcept {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }

        socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) [[unlikely]] return false;

        server_address_.sin_port = port_be16;
        server_address_.sin_addr = ip_be32;

        return ::connect(socket_fd_, &server_address_, sizeof(server_address_)) == 0;
    }

    bool ship_log(const void* log_data, std::uint32_t length, std::uint32_t flags = 0) noexcept {
        if (socket_fd_ < 0 || log_data == nullptr || length == 0) [[unlikely]] return false;

        PacketHeader header;
        header.length = length;
        header.flags = flags;

        std::array<iovec, 2> io_vectors;
        io_vectors[0].iov_base = &header;
        io_vectors[0].iov_len = sizeof(PacketHeader);
        io_vectors[1].iov_base = const_cast<void*>(log_data);
        io_vectors[1].iov_len = length;

        std::size_t total_bytes_to_send = sizeof(PacketHeader) + length;
        std::size_t total_bytes_sent = 0;

        // Maintain an explicit, sliding window reference over the vectors
        std::size_t v_idx = 0;
        std::size_t v_count = 2;

        while (total_bytes_sent < total_bytes_to_send) {
            long bytes_written = ::writev(socket_fd_, &io_vectors[v_idx], static_cast<int>(v_count));
            if (bytes_written <= 0) [[unlikely]] {
                return false; 
            }

            total_bytes_sent += bytes_written;

            if (total_bytes_sent < total_bytes_to_send) [[unlikely]] {
                std::size_t consumed = static_cast<std::size_t>(bytes_written);
                
                // Deterministic, non-destructive vector window manipulation
                while (consumed > 0 && v_count > 0) {
                    if (consumed >= io_vectors[v_idx].iov_len) {
                        consumed -= io_vectors[v_idx].iov_len;
                        v_idx++;
                        v_count--;
                    } else {
                        io_vectors[v_idx].iov_base = static_cast<char*>(io_vectors[v_idx].iov_base) + consumed;
                        io_vectors[v_idx].iov_len -= consumed;
                        consumed = 0;
                    }
                }
            }
        }
        return true;
    }

    auto async_ship(const void* log_data, std::uint32_t length, std::uint32_t flags = 0) noexcept {
        return [this, log_data, length, flags]() noexcept {
            return this->ship_log(log_data, length, flags);
        };
    }
};
