#pragma once

#include "diagnostic.hpp"
#include "voice.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace csy {

class BuildLog {
public:
    static BuildLog& instance() {
        static BuildLog log;
        return log;
    }

    bool open(const std::string& path, std::string& why) {
        if (path.empty()) { why = "the log file name is empty"; return false; }
        if (std::filesystem::path(path).extension() == ".csy") {
            why = "'" + path + "' ends in .csy, which is reserved for source files";
            return false;
        }
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0 && !S_ISREG(st.st_mode)) {
            why = "'" + path + "' exists and is not a regular file";
            return false;
        }
        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_) { why = "cannot write '" + path + "'"; return false; }
        path_ = path;
        start_ = std::chrono::steady_clock::now();
        if (!registered_) {
            registered_ = true;
            std::atexit([]() { BuildLog::instance().finish(); });
        }
        return true;
    }

    bool active() const { return file_.is_open(); }

    void line(const std::string& text) {
        if (!file_.is_open()) return;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "[%9.1f ms] ", ms);
        file_ << stamp << text << "\n";
        file_.flush();
    }

    void note_diagnostics(const DiagBag& bag) {
        if (!file_.is_open()) return;
        for (const auto& d : bag.items()) {
            const bool is_error = d.kind == Diagnostic::Error;
            if (is_error) saw_error_ = true;
            std::string where = d.file;
            if (d.line > 0) where += ":" + std::to_string(d.line) + ":" + std::to_string(d.col);
            const VoicedMessage voiced = voice_message(d.message);
            line(std::string(is_error ? "error   " : "warning ") + where + "  " + voiced.text);
            if (!voiced.fix.empty()) line("        fix: " + voiced.fix);
            for (const auto& step : d.trail) line("        " + step);
        }
        if (bag.suppressed_count() > 0) line(std::to_string(bag.suppressed_count()) + " more diagnostic(s) were not printed");
        if (bag.has_errors()) saw_error_ = true;
    }

    void mark_failed() { saw_error_ = true; }

private:
    void finish() {
        if (!file_.is_open()) return;
        line(saw_error_ ? "finished with errors" : "finished");
        file_.close();
        if (saw_error_) std::cerr << "  the full build log is in " << path_ << "\n";
    }

    std::ofstream file_;
    std::string path_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    bool saw_error_ = false;
    bool registered_ = false;
};

}
