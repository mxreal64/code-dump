#pragma once
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <algorithm>

struct MouseState {
    int x = 960;
    int y = 540;
    bool left_clicked = false;
    bool right_clicked = false;
};

class Y_Mouse {
private:
    int mouse_fd = -1;
    MouseState state;

public:
    bool init() {
       
        mouse_fd = open("/dev/input/mice", O_RDONLY | O_NONBLOCK);
        return mouse_fd >= 0;
    }

    void poll_events(int max_w, int max_h) {
        if (mouse_fd < 0) return;

        uint8_t data[3];
        if (read(mouse_fd, data, sizeof(data)) == 3) {
            state.left_clicked  = (data[0] & 0x01);
            state.right_clicked = (data[0] & 0x02);

           
            int8_t delta_x = static_cast<int8_t>(data[1]);
            int8_t delta_y = static_cast<int8_t>(data[2]);

           
            state.x += delta_x;
            state.y -= delta_y;

           
            state.x = std::clamp(state.x, 0, max_w - 1);
            state.y = std::clamp(state.y, 0, max_h - 1);
        }
    }

    [[nodiscard]] MouseState get_state() const { return state; }

    ~Y_Mouse() {
        if (mouse_fd >= 0) close(mouse_fd);
    }
};
