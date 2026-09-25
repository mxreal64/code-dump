module; // Global module fragment

#include <cstdio>
#include <string>
#include <concepts>
#include <format>
#include <charconv>

export module scan;

export namespace scan {
    template <typename T>
    concept ParsablePrimitive = std::same_as<T, std::string> ||
    std::same_as<T, char> ||
    std::integral<T> ||
    std::floating_point<T>;
}

namespace scan_impl {
    inline void write_raw_string(std::string_view str) {
        if (!str.empty()) {
            std::fwrite(str.data(), sizeof(char), str.size(), stdout);
            std::fflush(stdout);
        }
    }

    inline std::string read_raw_line() {
        std::string line;
        constexpr size_t buffer_size = 1024;
        char buffer[buffer_size];
        while (std::fgets(buffer, buffer_size, stdin)) {
            line += buffer;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                break;
            }
        }
        return line;
    }

    template <scan::ParsablePrimitive T>
    inline bool parse_view_token(std::string_view token, T& variable) {
        if constexpr (std::same_as<T, std::string>) {
            variable = std::string(token);
            return true;
        }
        else if constexpr (std::same_as<T, char>) {
            if (token.size() == 1) {
                variable = token[0];
                return true;
            }
        }
        else if constexpr (std::integral<T> || std::floating_point<T>) {
            if (!token.empty()) {
                const char* first = token.data();
                const char* last = first + token.size();
                auto result = [&]() {
                    if constexpr (std::floating_point<T>) {
                        return std::from_chars(first, last, variable, std::chars_format::general);
                    } else {
                        return std::from_chars(first, last, variable);
                    }
                }();
                return (result.ec == std::errc{} && result.ptr == last);
            }
        }
        return false;
    }
}

export namespace scan {
    template <ParsablePrimitive T, typename... Args>
    void into(T& variable, std::format_string<Args...> prompt_fmt, Args&&... args) {
        std::string prompt = std::vformat(prompt_fmt.get(), std::make_format_args(args...));

        while (true) {
            scan_impl::write_raw_string(prompt);
            std::string input_str = scan_impl::read_raw_line();

            if (scan_impl::parse_view_token(std::string_view(input_str), variable)) {
                return;
            }
            scan_impl::write_raw_string("Invalid input type. Please try again.\n");
        }
    }

    template <ParsablePrimitive T, typename... Args>
    [[nodiscard]] auto value(std::format_string<Args...> prompt_fmt, Args&&... args) -> T {
        T local_variable{};
        into(local_variable, prompt_fmt, std::forward<Args>(args)...);
        return local_variable;
    }

    inline void any_key(std::string_view prompt = "Press Enter to continue...") {
        scan_impl::write_raw_string(prompt);
        (void)scan_impl::read_raw_line();
    }

    template <ParsablePrimitive... Types>
    bool into_all(char delimiter, Types&... variables) {
        std::string input_str = scan_impl::read_raw_line();
        std::string_view view(input_str);
        size_t current_pos = 0;

        auto parse_next = [&](auto& var) -> bool {
            if (current_pos > view.size()) return false;
            size_t next_delim = view.find(delimiter, current_pos);

            std::string_view token = (next_delim == std::string_view::npos)
            ? view.substr(current_pos)
            : view.substr(current_pos, next_delim - current_pos);

            current_pos = (next_delim == std::string_view::npos) ? view.size() + 1 : next_delim + 1;
            return scan_impl::parse_view_token(token, var);
        };

        bool success = (parse_next(variables) && ...);
        if (current_pos <= view.size()) return false;
        return success;
    }

    template <ParsablePrimitive... Types>
    [[nodiscard]] auto as_tuple(char delimiter, std::string_view prompt = "") -> std::tuple<Types...> {
        while (true) {
            scan_impl::write_raw_string(prompt);
            std::tuple<Types...> result_tuple{};

            // Unpack tuple references into into_all
            bool success = std::apply([&](auto&... args) {
                return into_all(delimiter, args...);
            }, result_tuple);

            if (success) {
                return result_tuple;
            }
            scan_impl::write_raw_string("Invalid multi-value format. Please try again.\n");
        }
    }
}
