// File: src/terminal.cppm
export module kernel86.terminal;

import kernel86.types;

export namespace kernel86::terminal {

    enum class Color : uint8_t {
        Black = 0, Blue = 1, Green = 2, Cyan = 3,
        Red = 4, Magenta = 5, Brown = 6, LightGray = 7,
        DarkGray = 8, LightBlue = 9, LightGreen = 10,
        LightCyan = 11, LightRed = 12, Pink = 13, Yellow = 14, White = 15
    };

    class Console {
    private:
        static constexpr size_t Width = 80;
        static constexpr size_t Height = 25;
        uint16_t* const vga_buffer = reinterpret_cast<uint16_t*>(0xB8000);

        size_t row{0};
        size_t col{0};
        uint8_t current_attribute{0x0F}; // White on Black default

    public:
        constexpr Console() noexcept = default;

        void SetColor(Color fg, Color bg) noexcept {
            current_attribute = static_cast<uint8_t>(fg) | (static_cast<uint8_t>(bg) << 4);
        }

        void Clear() noexcept {
            uint16_t blank = ' ' | (current_attribute << 8);
            for (size_t i = 0; i < Width * Height; ++i) {
                vga_buffer[i] = blank;
            }
            row = 0;
            col = 0;
        }

        void WriteChar(char c) noexcept {
            if (c == '\n') {
                col = 0;
                row++;
                return;
            }

            if (col >= Width) {
                col = 0;
                row++;
            }

            if (row >= Height) {
                // Future point: Implement scrolling. For now, wrap around to top
                row = 0;
            }

            size_t index = row * Width + col;
            vga_buffer[index] = static_cast<uint16_t>(c) | (static_cast<uint16_t>(current_attribute) << 8);
            col++;
        }

        void Write(const char* str) noexcept {
            while (str && *str) {
                WriteChar(*str);
                str++;
            }
        }
    };
}
