import std;
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

class [[nodiscard]] TerminalGuard {
    struct termios orig;
public:
    TerminalGuard() {
        if (::tcgetattr(STDIN_FILENO, &orig) == 0) {
            auto raw = orig;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG);
            raw.c_iflag &= ~(IXON | ICRNL);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
    }
    ~TerminalGuard() { ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig); }
};

struct TextEngine {
    std::list<std::string> buffer{""};
    std::list<std::string>::iterator cur_line = buffer.begin();
    size_t col = 0, row = 0, scroll_row = 0;
    std::filesystem::path filepath = std::filesystem::current_path() / "untitled.txt";
    bool is_dirty = false;
    std::string msg = "Ctrl+S: Save | Ctrl+Q: Exit";

    auto win_size() const noexcept -> std::pair<size_t, size_t> {
        struct winsize ws;
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col != 0) {
            return {static_cast<size_t>(ws.ws_row), static_cast<size_t>(ws.ws_col)};
        }
        return {24, 80};
    }

    void scroll_to_cursor(size_t visible_rows) {
        if (row < scroll_row) scroll_row = row;
        if (row >= scroll_row + visible_rows) scroll_row = row - visible_rows + 1;
    }

    void refresh() {
        auto [rows, cols] = win_size();
        size_t txt_rows = rows - 4;
        scroll_to_cursor(txt_rows);

        std::print("\x1b[?25l\x1b[H\x1b[J");

       
        std::string top_text = std::format(" Editing: {} ", filepath.string());
        std::print("\x1b[1;44;37m{:<{}}\x1b[m\r\n\r\n", top_text, cols);

       
        int gutter_w = std::max(2, static_cast<int>(std::to_string(buffer.size()).length())) + 1;
        auto it = std::next(buffer.begin(), scroll_row);
        
        for (size_t i = 0; i < txt_rows; ++i) {
            if (it != buffer.end()) {
                size_t abs_line = scroll_row + i + 1;
                std::print("\r\x1b[{}{} {:>{}} \x1b[m{}", (abs_line - 1 == row) ? "33m" : "90m", (abs_line - 1 == row) ? "*" : " ", abs_line, gutter_w - 2, *it);
                it = std::next(it);
            }
            std::println("\r");
        }

       
        std::string stat_l = std::format(" Lines: {} / {}", row + 1, buffer.size());
        std::string stat_r = std::format("Col {} {}", col + 1, is_dirty ? "[Modified]" : "");
        size_t pad = (cols > stat_l.length() + stat_r.length()) ? cols - stat_l.length() - stat_r.length() : 0;
        std::print("\x1b[7m{}{:<{}}{}\x1b[m\r\n{}", stat_l, "", pad, stat_r, msg);

       
        std::print("\x1b[{};{}H\x1b[?25h", (row - scroll_row) + 3, col + gutter_w + 1);
        std::flush(std::cout);
    }

    void handle_bs() {
        if (col > 0) {
            cur_line->erase(--col, 1);
            is_dirty = true;
        } else if (cur_line != buffer.begin()) {
            auto prev = std::prev(cur_line);
            col = prev->length();
            *prev += *cur_line;
            buffer.erase(cur_line);
            cur_line = prev;
            row--;
            is_dirty = true;
        }
    }

    void handle_enter() {
        cur_line = buffer.insert(std::next(cur_line), cur_line->substr(col));
        cur_line = std::prev(cur_line);
        cur_line->erase(col);
        cur_line = std::next(cur_line);
        row++; col = 0;
        is_dirty = true;
    }

    void move_left() {
        if (col > 0) col--;
        else if (cur_line != buffer.begin()) { cur_line = std::prev(cur_line); row--; col = cur_line->length(); }
    }
    void move_right() {
        if (col < cur_line->length()) col++;
        else if (std::next(cur_line) != buffer.end()) { cur_line = std::next(cur_line); row++; col = 0; }
    }
    void move_up() { if (cur_line != buffer.begin()) { cur_line = std::prev(cur_line); row--; col = std::min(col, cur_line->length()); } }
    void move_down() { if (std::next(cur_line) != buffer.end()) { cur_line = std::next(cur_line); row++; col = std::min(col, cur_line->length()); } }

    void save() {
        if (std::ofstream out(filepath); out) {
            for (const auto& l : buffer) out << l << "\n";
            is_dirty = false; msg = "Saved successfully!";
        } else msg = "Error: Save failed!";
    }
};

int main(int argc, char* argv[]) {
    TerminalGuard guard;
    TextEngine eng;

    if (argc > 1) {
       
        eng.filepath = std::filesystem::absolute(argv[1]);
        if (std::ifstream in(eng.filepath); in) {
            eng.buffer.clear();
            for (std::string line; std::getline(in, line);) eng.buffer.push_back(line);
            if (eng.buffer.empty()) eng.buffer.push_back("");
            eng.cur_line = eng.buffer.begin();
        }
    }

    while (true) {
        eng.refresh();
        char c;
        if (::read(STDIN_FILENO, &c, 1) <= 0) break;

        if (c == 17) {
            if (eng.is_dirty) { eng.msg = "Unsaved changes! Press Ctrl+Q again to force quit."; eng.is_dirty = false; continue; }
            break;
        }
        if (c == 19) { eng.save(); continue; }

        switch (c) {
            case 127: eng.handle_bs(); break;
            case '\r':
            case '\n': eng.handle_enter(); break;
            case 27: {
                char seq;
                if (::read(STDIN_FILENO, &seq, 1) > 0 && ::read(STDIN_FILENO, &seq, 1) > 0 && seq == '[') {
                    switch (seq) {
                        case 'A': eng.move_up(); break;
                        case 'B': eng.move_down(); break;
                        case 'C': eng.move_right(); break;
                        case 'D': eng.move_left(); break;
                    }
                }
                break;
            }
            default:
                if (!std::iscntrl(static_cast<unsigned char>(c))) {
                    eng.cur_line->insert(eng.col++, 1, c);
                    eng.is_dirty = true;
                }
                break;
        }
    }
    std::print("\x1b[2J\x1b[H");
    return 0;
}
