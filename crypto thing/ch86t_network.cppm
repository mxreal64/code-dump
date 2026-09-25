export module ch86t.network;

import ch86t.crypto;
import ch86t.queue;
import ch86t.storage;
import <string>;
import <thread>;
import <atomic>;
import <chrono>;
import <vector>;
import <sys/socket.h>;
import <netinet/in.h>;
import <unistd.h>;
import <fcntl.h>;
import <signal.h>;

export namespace ch86t::network {

    export class NetworkEngine {
    private:
        int server_fd{-1}; int client_fd{-1};
        std::atomic<bool> is_running{false}; std::thread net_thread;
        std::atomic<uint32_t> global_requests{0}; uint64_t last_reset{0};

        void check_global_fuse() {
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now - last_reset > 1000) { last_reset = now; global_requests.store(0, std::memory_order_relaxed); }
            if (global_requests.fetch_add(1, std::memory_order_relaxed) > 100) kill(getpid(), SIGTERM);
        }

        void listen_and_pump(ch86t::queue::SPSCQueue<std::string, 256>* to_ui, ch86t::queue::SPSCQueue<std::string, 256>* from_ui, ch86t::storage::UserProfile local_user) {
            struct sockaddr_in c_addr{}; socklen_t c_len = sizeof(c_addr);
            int infd = accept(server_fd, (struct sockaddr*)&c_addr, &c_len); if (infd < 0) return;
            fcntl(infd, F_SETFL, fcntl(infd, F_GETFL, 0) | O_NONBLOCK); client_fd = infd;

            std::string hello = "HELLO " + local_user.fingerprint + " " + ch86t::crypto::to_hex(local_user.x_pub) + "\n";
            send(client_fd, hello.data(), hello.size(), MSG_NOSIGNAL);

            std::string line; std::vector<uint8_t> p_pub; std::string p_print; char buf[1024];

            while (is_running.load()) {
                check_global_fuse();
                ssize_t bytes = recv(client_fd, buf, sizeof(buf), 0);
                if (bytes > 0) {
                    for (ssize_t i = 0; i < bytes; ++i) {
                        if (buf[i] != '\n') { line += buf[i]; }
                        else {
                            if (line.rfind("HELLO ", 0) == 0) {
                                size_t s1 = line.find(' '); size_t s2 = line.find(' ', s1 + 1);
                                p_print = line.substr(s1 + 1, s2 - s1 - 1); p_pub = ch86t::crypto::from_hex(line.substr(s2 + 1));
                                if (to_ui) to_ui->push("[mDNS]" + p_print);
                            } else if (line.rfind("DATA ", 0) == 0 && !p_pub.empty()) {
                                size_t s1 = line.find(' '); size_t s2 = line.find(' ', s1 + 1);
                                ch86t::crypto::EncryptedPacket pkt{.nonce = ch86t::crypto::from_hex(line.substr(s1 + 1, s2 - s1 - 1)), .ciphertext = ch86t::crypto::from_hex(line.substr(s2 + 1))};
                                if (to_ui) to_ui->push("[MSG]" + p_print + ": " + ch86t::crypto::decrypt_payload(pkt, p_pub, local_user.x_sec));
                            }
                            line.clear();
                        }
                    }
                } else if (bytes == 0) { break; }

                std::string out;
                if (from_ui && from_ui->pop(out) && out.rfind("OUT|", 0) == 0 && !p_pub.empty()) {
                    auto pkt = ch86t::crypto::encrypt_payload(out.substr(4), p_pub, local_user.x_sec);
                    std::string wire = "DATA " + ch86t::crypto::to_hex(pkt.nonce) + " " + ch86t::crypto::to_hex(pkt.ciphertext) + "\n";
                    send(client_fd, wire.data(), wire.size(), MSG_NOSIGNAL);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

    public:
        NetworkEngine() = default;
        bool start(const ch86t::storage::UserProfile& user, uint16_t port, ch86t::queue::SPSCQueue<std::string, 256>* t_ui, ch86t::queue::SPSCQueue<std::string, 256>* f_ui) {
            server_fd = socket(AF_INET, SOCK_STREAM, 0); if (server_fd < 0) return false;
            int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr{.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {INADDR_ANY}};
            if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_fd, 1) < 0) return false;
            is_running.store(true); net_thread = std::thread(&NetworkEngine::listen_and_pump, this, t_ui, f_ui, user);
            return true;
        }
        void stop() { is_running.store(false); if (net_thread.joinable()) net_thread.join(); if (client_fd >= 0) close(client_fd); if (server_fd >= 0) close(server_fd); }
    };
}
