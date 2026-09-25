// File: graphics.cppm
export module kernel.graphics;

import kernel.core_types;

export namespace kernel::graphics {

    struct Color {
        uint8_t blue;
        uint8_t green;
        uint8_t red;
    };

    class Framebuffer {
    private:
        // Standard VGA Text Mode Memory Pointer
        uint16_t* const text_memory = reinterpret_cast<uint16_t*>(0xB8000);

    public:
        [[gnu::always_inline]] inline void Clear(Color) noexcept {
            // Write "UNIX" in bright green text right onto the screen
            // 0x0A is the VGA attribute byte for Light Green
            text_memory[320] = 'U' | (0x0A << 8);
            text_memory[321] = 'N' | (0x0A << 8);
            text_memory[322] = 'I' | (0x0A << 8);
            text_memory[323] = 'X' | (0x0A << 8);
            text_memory[324] = ' ' | (0x0A << 8);
            text_memory[325] = '2' | (0x0A << 8);
            text_memory[326] = '.' | (0x0A << 8);
            text_memory[327] = '0' | (0x0A << 8);
        }
    };
}
