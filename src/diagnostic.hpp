#pragma once

#include "lexer.hpp"
#include <string>
#include <vector>
#include <sstream>

namespace csy {

struct Diagnostic {
    enum Kind { Error, Warning, Note };
    Kind kind;
    std::string file;
    int line;
    int col;
    std::string message;
    std::vector<std::string> notes;
};

class DiagBag {
public:
    static constexpr size_t kMaxStoredErrors = 100;
    static constexpr size_t kMaxStoredWarnings = 200;

    void error(const std::string& file, int line, int col, const std::string& msg) {
        ++errors_;
        if (stored_errors_ >= kMaxStoredErrors) { last_stored_ = false; ++suppressed_; return; }
        ++stored_errors_;
        last_stored_ = true;
        diags_.push_back(Diagnostic{Diagnostic::Error, file, line, col, msg, {}});
    }
    void warn(const std::string& file, int line, int col, const std::string& msg) {
        ++warnings_;
        if (stored_warnings_ >= kMaxStoredWarnings) { last_stored_ = false; ++suppressed_; return; }
        ++stored_warnings_;
        last_stored_ = true;
        diags_.push_back(Diagnostic{Diagnostic::Warning, file, line, col, msg, {}});
    }
    void note(const std::string& msg) {
        if (last_stored_ && !diags_.empty()) diags_.back().notes.push_back(msg);
    }
    bool has_errors() const { return errors_ > 0; }
    bool error_limit_reached() const { return errors_ >= kMaxStoredErrors; }
    size_t error_count() const { return errors_; }
    size_t warning_count() const { return warnings_; }
    size_t suppressed_count() const { return suppressed_; }
    bool empty() const { return diags_.empty() && suppressed_ == 0; }
    const std::vector<Diagnostic>& items() const { return diags_; }

    std::string format(const std::string& source) const;

private:
    std::vector<Diagnostic> diags_;
    size_t errors_ = 0, warnings_ = 0, suppressed_ = 0;
    size_t stored_errors_ = 0, stored_warnings_ = 0;
    bool last_stored_ = false;
};

std::string format_diag(const Diagnostic& d, const std::string& source,
                        const std::vector<size_t>* line_starts = nullptr);

std::string get_line(const std::string& source, int line);

}
