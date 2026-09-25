module;
#include "FTXUI/include/ftxui/component/component.hpp"
#include "FTXUI/include/ftxui/component/screen_interactive.hpp"
#include "FTXUI/include/ftxui/dom/elements.hpp"

export module ch86t.ui;

import ch86t.queue;
import <string>;
import <vector>;
import <algorithm>;
import <atomic>;
import <unistd.h>;

using namespace ftxui;

export namespace ch86t::ui {

    class TerminalInterface {
    private:
        std::string input_buffer; std::string local_fingerprint;
        std::vector<std::string> chat_log; std::vector<std::string> connected_peers;
        ch86t::queue::SPSCQueue<std::string, 256>* inbound{nullptr};
        ch86t::queue::SPSCQueue<std::string, 256>* outbound{nullptr};

        void drain_queue() {
            if (!inbound) return;
            std::string inc;
            while (inbound->pop(inc)) {
                if (inc.rfind("[mDNS]", 0) == 0) {
                    std::string label = inc.substr(6);
                    if (std::find(connected_peers.begin(), connected_peers.end(), label) == connected_peers.end()) connected_peers.push_back(label);
                } else if (inc.rfind("[MSG]", 0) == 0) { chat_log.push_back(inc.substr(5)); }
            }
        }

    public:
        TerminalInterface(std::string fp, ch86t::queue::SPSCQueue<std::string, 256>* n_to_u, ch86t::queue::SPSCQueue<std::string, 256>* u_to_n)
            : local_fingerprint(std::move(fp)), inbound(n_to_u), outbound(u_to_n) {}

        void run_active_loop() {
            auto screen = ScreenInteractive::Fullscreen();
            Component box = Input(&input_buffer, "Message...");
            auto render = Renderer(box, [&] {
                drain_queue();
                Elements peers; for (const auto& p : connected_peers) peers.push_back(text("> " + p) | color(Color::Green));
                Elements logs; for (const auto& m : chat_log) logs.push_back(text(m));
                return hbox({
                    vbox({ window(text("PEERS"), vbox(std::move(peers))) | flex, window(text("GAMES"), vbox({text(" Pong (ASCII)")})), text(" " + local_fingerprint) | dim }) | size(WIDTH, EQUAL, 24),
                    vbox({ window(text("# general-chat"), vbox(std::move(logs))) | flex, hbox({ text("> "), box->Render() }) | border }) | flex
                });
            });

            auto router = CatchEvent(render, [&](Event ev) {
                if (ev == Event::Escape) { screen.ExitLoopClosure()(); return true; }
                if (ev == Event::Return && !input_buffer.empty()) {
                    if (input_buffer == "/kill") _exit(EXIT_FAILURE);
                    chat_log.push_back("[Me] " + input_buffer);
                    if (outbound) outbound->push("OUT|" + input_buffer);
                    input_buffer.clear(); return true;
                }
                return false;
            });
            screen.Loop(router);
        }
    };
}
