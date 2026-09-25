export module ch86t.storage;

import <string>;
import <vector>;
import <string_view>;
import <fcntl.h>;
import <unistd.h>;
import <cstdlib>;

export namespace ch86t::storage {

    struct UserProfile {
        std::string username; std::string fingerprint;
        std::vector<uint8_t> ed_pub; std::vector<uint8_t> ed_sec;
        std::vector<uint8_t> x_pub; std::vector<uint8_t> x_sec;
    };

    std::string get_config_path() {
        const char* home = std::getenv("HOME");
        return (home ? std::string(home) : ".") + "/.config/ch86t/identity.dat";
    }

    bool profile_exists() { return access(get_config_path().c_str(), F_OK) == 0; }

    void save_profile(const UserProfile& profile) {
        const char* home = std::getenv("HOME");
        std::string cmd = "mkdir -p " + (home ? std::string(home) : ".") + "/.config/ch86t";
        std::system(cmd.c_str());

        int fd = open(get_config_path().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return;

        write(fd, "CH86T", 5);
        uint8_t version = 2; write(fd, &version, 1);

        auto write_f = [](int file_fd, const auto& container) {
            size_t len = container.size();
            write(file_fd, &len, sizeof(len));
            write(file_fd, container.data(), len);
        };

        write_f(fd, profile.username); write_f(fd, profile.fingerprint);
        write_f(fd, profile.ed_pub); write_f(fd, profile.ed_sec);
        write_f(fd, profile.x_pub); write_f(fd, profile.x_sec);
        close(fd);
    }

    UserProfile load_profile() {
        UserProfile p;
        int fd = open(get_config_path().c_str(), O_RDONLY);
        if (fd < 0) return p;

        char magic[5];
        if (read(fd, magic, 5) == 5 && std::string_view(magic, 5) == "CH86T") {
            uint8_t v = 0; read(fd, &v, 1);
            auto r_str = [](int file_fd) {
                size_t len = 0; read(file_fd, &len, sizeof(len));
                std::string s(len, '\0'); read(file_fd, s.data(), len);
                return s;
            };
            auto r_vec = [](int file_fd) {
                size_t len = 0; read(file_fd, &len, sizeof(len));
                std::vector<uint8_t> v(len); read(file_fd, v.data(), len);
                return v;
            };
            p.username = r_str(fd); p.fingerprint = r_str(fd);
            p.ed_pub = r_vec(fd); p.ed_sec = r_vec(fd);
            p.x_pub = r_vec(fd); p.x_sec = r_vec(fd);
        }
        close(fd); return p;
    }
}
