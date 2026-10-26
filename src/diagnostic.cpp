#include "diagnostic.hpp"
#include <sstream>
#include <algorithm>

namespace csy {

std::string get_line(const std::string& source, int line) {
    if (line < 1) return "";
    int cur = 1;
    size_t start = 0;
    for (size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\n') {
            if (cur == line) {
                return source.substr(start, i - start);
            }
            cur++;
            start = i + 1;
        }
    }
    if (cur == line) {
        return source.substr(start);
    }
    return "";
}

static const char* kind_str(Diagnostic::Kind k) {
    switch (k) {
        case Diagnostic::Error: return "error";
        case Diagnostic::Warning: return "warning";
        case Diagnostic::Note: return "note";
    }
    return "?";
}

static std::vector<size_t> build_line_index(const std::string& source) {
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

static std::string line_from_index(const std::string& source, const std::vector<size_t>& starts, int line) {
    if (line < 1 || static_cast<size_t>(line) > starts.size()) return "";
    const size_t begin = starts[line - 1];
    size_t end = static_cast<size_t>(line) < starts.size() ? starts[line] - 1 : source.size();
    if (end < begin) end = begin;
    return source.substr(begin, end - begin);
}

std::string format_diag(const Diagnostic& d, const std::string& source,
                        const std::vector<size_t>* line_starts) {
    std::stringstream ss;
    ss << "\n";
    ss << "  " << d.file << ":" << d.line << ":" << d.col << "  " << kind_str(d.kind) << "\n";
    ss << "  " << std::string(40, '-') << "\n";
    ss << "  " << d.message << "\n";
    std::string line_text = line_starts ? line_from_index(source, *line_starts, d.line)
                                        : get_line(source, d.line);
    if (line_text.size() > 400) line_text = line_text.substr(0, 400) + " ...";
    if (!line_text.empty()) {
        ss << "\n  " << d.line << " | " << line_text << "\n";
        std::string caret_pad(std::to_string(d.line).size() + 4 + (d.col > 0 ? d.col - 1 : 0), ' ');
        caret_pad += "^";
        ss << "  " << caret_pad << "\n";
    }
    for (const auto& n : d.notes) {
        ss << "  hint: " << n << "\n";
    }
    ss << "\n";
    return ss.str();
}

std::string DiagBag::format(const std::string& source) const {
    std::stringstream ss;
    const std::vector<size_t> starts = diags_.empty() ? std::vector<size_t>{} : build_line_index(source);
    for (const auto& d : diags_) {
        ss << format_diag(d, source, &starts);
    }
    if (suppressed_ > 0) {
        ss << "\n  ... " << suppressed_ << " more diagnostic(s) not shown (only the first "
           << kMaxStoredErrors << " errors and " << kMaxStoredWarnings << " warnings are printed)\n";
    }
    if (errors_ || warnings_) {
        ss << "  " << errors_ << " error(s), " << warnings_ << " warning(s)\n";
    }
    return ss.str();
}

}
