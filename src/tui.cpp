
#include "tui.hpp"
#include "value.hpp"
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>

namespace csy {
namespace {

struct termios g_orig_termios;
bool g_raw_active = false;
bool g_cleanup_registered = false;

void restore_terminal() {
    if (g_raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_raw_active = false;
    }
    std::cout << "\x1b[?25h";
    std::cout << "\x1b[?1049l";
    std::cout << "\x1b[0m";
    std::cout.flush();
}

void on_signal(int sig) {
    restore_terminal();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void ensure_cleanup_registered() {
    if (g_cleanup_registered) return;
    g_cleanup_registered = true;
    std::atexit(restore_terminal);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

void write_esc(const std::string& seq) {
    std::cout << seq;
}

}

void register_tui_builtins(VM& vm) {
    auto& builtins = vm.builtins_table();

    builtins["tui_clear"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[2J\x1b[H");
        return Value::make_none();
    };

    builtins["tui_goto"] = [](VM&, std::vector<Value>& args) -> Value {
        long long row = args.size() > 0 ? args[0].int_val : 1;
        long long col = args.size() > 1 ? args[1].int_val : 1;
        write_esc("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H");
        return Value::make_none();
    };

    builtins["tui_hide_cursor"] = [](VM&, std::vector<Value>&) -> Value {
        ensure_cleanup_registered();
        write_esc("\x1b[?25l");
        return Value::make_none();
    };
    builtins["tui_show_cursor"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[?25h");
        return Value::make_none();
    };

    builtins["tui_alt_screen_on"] = [](VM&, std::vector<Value>&) -> Value {
        ensure_cleanup_registered();
        write_esc("\x1b[?1049h");
        return Value::make_none();
    };
    builtins["tui_alt_screen_off"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[?1049l");
        return Value::make_none();
    };

    builtins["tui_color"] = [](VM&, std::vector<Value>& args) -> Value {
        if (!args.empty()) {
            long long code = args[0].type == Value::Int ? args[0].int_val : (long long)args[0].float_val;
            write_esc("\x1b[" + std::to_string(code) + "m");
        }
        return Value::make_none();
    };
    builtins["tui_bold"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[1m");
        return Value::make_none();
    };
    builtins["tui_dim"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[2m");
        return Value::make_none();
    };
    builtins["tui_reset"] = [](VM&, std::vector<Value>&) -> Value {
        write_esc("\x1b[0m");
        return Value::make_none();
    };

    auto rgb_seq = [](int base, std::vector<Value>& args) -> std::string {
        auto comp = [&](size_t i) -> int {
            if (i >= args.size()) return 0;
            long long v = args[i].type == Value::Int ? args[i].int_val : (long long)args[i].float_val;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            return (int)v;
        };
        std::ostringstream ss;
        ss << "\x1b[" << base << ";2;" << comp(0) << ";" << comp(1) << ";" << comp(2) << "m";
        return ss.str();
    };
    builtins["tui_rgb"] = [rgb_seq](VM&, std::vector<Value>& args) -> Value {
        write_esc(rgb_seq(38, args));
        return Value::make_none();
    };
    builtins["tui_bg_rgb"] = [rgb_seq](VM&, std::vector<Value>& args) -> Value {
        write_esc(rgb_seq(48, args));
        return Value::make_none();
    };

    builtins["tui_flush"] = [](VM&, std::vector<Value>&) -> Value {
        std::cout.flush();
        return Value::make_none();
    };

    builtins["tui_rows"] = [](VM&, std::vector<Value>&) -> Value {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return Value::make_int(w.ws_row);
        return Value::make_int(24);
    };
    builtins["tui_cols"] = [](VM&, std::vector<Value>&) -> Value {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return Value::make_int(w.ws_col);
        return Value::make_int(80);
    };
    builtins["tui_size"] = [](VM&, std::vector<Value>&) -> Value {
        struct winsize w;
        long long rows = 24, cols = 80;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
            rows = w.ws_row; cols = w.ws_col;
        }
        std::vector<Value> v = {Value::make_int(rows), Value::make_int(cols)};
        return Value::make_vec(std::move(v));
    };

    builtins["tui_raw_on"] = [](VM&, std::vector<Value>&) -> Value {
        ensure_cleanup_registered();
        if (!g_raw_active) {
            if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
                struct termios raw = g_orig_termios;
                raw.c_lflag &= ~(ICANON | ECHO);
                raw.c_cc[VMIN] = 0;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                    g_raw_active = true;
                }
            }
        }
        return Value::make_none();
    };
    builtins["tui_raw_off"] = [](VM&, std::vector<Value>&) -> Value {
        if (g_raw_active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
            g_raw_active = false;
        }
        return Value::make_none();
    };

    builtins["tui_key"] = [](VM&, std::vector<Value>&) -> Value {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv = {0, 0};
        int r = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (r > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n == 1) return Value::make_str(std::string(1, c));
        }
        return Value::make_str("");
    };
}

}

