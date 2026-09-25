#pragma once
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mdspan> 
#include <vector>
#include <cstdint>
#include <execution>
#include <algorithm>
#include <numeric>

struct Window {
    pid_t pid;
    int x, y, width, height;
    uint32_t* buffer_ptr = nullptr;
    bool visible = true;
};

class Y_Engine {
private:
    int fb_fd = -1;
    uint32_t* fb_ptr = nullptr;
    long screen_size = 0;
    
public:
    size_t canvas_width = 1920; 
    size_t canvas_height = 1080;
    bool secure_uac_active = false;
    std::vector<Window> window_stack;

    bool init() {
       
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd < 0) return false;

        fb_var_screeninfo vinfo;
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
            close(fb_fd);
            return false;
        }

        canvas_width = vinfo.xres;
        canvas_height = vinfo.yres;
        screen_size = vinfo.xres * vinfo.yres * sizeof(uint32_t);

        void* map = mmap(0, screen_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
        if (map == MAP_FAILED) {
            close(fb_fd);
            return false;
        }
        
        fb_ptr = static_cast<uint32_t*>(map);
        return true;
    }

   
    auto get_canvas() noexcept {
        return std::mdspan<uint32_t, std::extents<size_t, std::dynamic_extent, std::dynamic_extent>, std::layout_right>(
            fb_ptr, canvas_height, canvas_width
        );
    }

   
    void render() noexcept {
        if (!fb_ptr) return;

        auto screen = get_canvas();

       
        for (const auto& win : window_stack) {
            if (!win.visible || !win.buffer_ptr) continue;

           
            auto win_matrix = std::mdspan<const uint32_t, std::extents<size_t, std::dynamic_extent, std::dynamic_extent>, std::layout_right>(
                win.buffer_ptr, win.height, win.width
            );

           
            int start_row = std::max(0, -win.y);
            int end_row = std::min(win.height, static_cast<int>(canvas_height) - win.y);
            int start_col = std::max(0, -win.x);
            int col_count = std::min(win.width, static_cast<int>(canvas_width) - win.x) - start_col;

            if (col_count <= 0) continue;

            for (int row = start_row; row < end_row; ++row) {
                size_t target_y = static_cast<size_t>(win.y + row);
                size_t target_x = static_cast<size_t>(win.x + start_col);
                
               
                std::copy_n(&win_matrix[row, start_col], col_count, &screen[target_y, target_x]);
            }
        }

       
        if (secure_uac_active) {
            execute_secure_box_blur();
            draw_consent_prompt_box();
        }
    }

    void trigger_consent_veto(bool active) noexcept {
        secure_uac_active = active;
    }

private:
   
    void execute_secure_box_blur() noexcept {
        auto screen = get_canvas();
        const int radius = 12;

        std::vector<size_t> rows(canvas_height);
        std::iota(rows.begin(), rows.end(), 0);

       
        std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [=, this](size_t y) {
            for (size_t x = 0; x < canvas_width; ++x) {
                uint32_t r = 0, g = 0, b = 0, count = 0;

                for (int k = -radius; k <= radius; ++k) {
                    int nx = static_cast<int>(x) + k;
                    if (nx >= 0 && nx < static_cast<int>(canvas_width)) {
                        uint32_t pixel = screen[y, static_cast<size_t>(nx)];
                        r += (pixel >> 16) & 0xFF;
                        g += (pixel >> 8) & 0xFF;
                        b += pixel & 0xFF;
                        count++;
                    }
                }
                screen[y, x] = ((r / count) << 16) | ((g / count) << 8) | (b / count);
            }
        });
    }

    void draw_consent_prompt_box() noexcept {
        auto screen = get_canvas();
        size_t box_w = 460;
        size_t box_h = 240;
        size_t start_x = (canvas_width - box_w) / 2;
        size_t start_y = (canvas_height - box_h) / 2;

        for (size_t y = start_y; y < start_y + box_h; ++y) {
            for (size_t x = start_x; x < start_x + box_w; ++x) {
                screen[y, x] = 0x1E1E2E;
            }
        }
    }

public:
    ~Y_Engine() {
        if (fb_ptr) munmap(fb_ptr, screen_size);
        if (fb_fd >= 0) close(fb_fd);
    }
};
