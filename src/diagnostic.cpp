#include "diagnostic.hpp"
#include "voice.hpp"
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

static bool is_control_at(const std::string& text, size_t i, size_t& width) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    width = 1;
    if (c == '\n' || c == '\t') return false;
    if (c < 0x20 || c == 0x7f) return true;
    if (c == 0xc2 && i + 1 < text.size()) {
        const unsigned char d = static_cast<unsigned char>(text[i + 1]);
        if (d >= 0x80 && d <= 0x9f) {
            width = 2;
            return true;
        }
    }
    return false;
}

static std::string scrub_excerpt(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t width = 1;
        if (is_control_at(text, i, width)) {
            out.push_back('?');
            i += width;
        } else {
            out.push_back(text[i]);
            i++;
        }
    }
    return out;
}

static std::string scrub_text(const std::string& text) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t width = 1;
        if (is_control_at(text, i, width)) {
            for (size_t k = 0; k < width; k++) {
                const unsigned char c = static_cast<unsigned char>(text[i + k]);
                out += "\\x";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            }
            i += width;
        } else {
            out.push_back(text[i]);
            i++;
        }
    }
    return out;
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

static std::string caret_padding(const std::string& line_text, int col) {
    std::string pad;
    const int limit = col > 0 ? col - 1 : 0;
    for (int i = 0; i < limit; i++) {
        const char c = static_cast<size_t>(i) < line_text.size() ? line_text[static_cast<size_t>(i)] : ' ';
        pad.push_back(c == '\t' ? '\t' : ' ');
    }
    return pad;
}

std::string format_diag(const Diagnostic& d, const std::string& source,
                        const std::vector<size_t>* line_starts) {
    std::stringstream ss;
    const VoicedMessage voiced = voice_message(d.message);
    ss << "\n";
    ss << "  " << d.file;
    if (d.line > 0) ss << ":" << d.line << ":" << d.col;
    ss << "  " << kind_str(d.kind) << "\n";
    ss << "  " << std::string(40, '-') << "\n";
    ss << "  " << voiced.text << "\n";
    auto fetch = [&](int line) {
        std::string text = line_starts ? line_from_index(source, *line_starts, line) : get_line(source, line);
        if (text.size() > 400) text = text.substr(0, 400) + " ...";
        return scrub_excerpt(text);
    };
    const std::string line_text = fetch(d.line);
    if (!line_text.empty()) {
        const std::string before = d.line > 1 ? fetch(d.line - 1) : std::string();
        const std::string after = fetch(d.line + 1);
        const size_t width = std::to_string(d.line + 1).size();
        auto number = [&](int n) {
            std::string label = std::to_string(n);
            return std::string(width - label.size(), ' ') + label;
        };
        ss << "\n";
        if (!before.empty()) ss << "  " << number(d.line - 1) << " | " << before << "\n";
        ss << "  " << number(d.line) << " | " << line_text << "\n";
        size_t span = d.length > 0 ? static_cast<size_t>(d.length) : 1;
        const size_t room = d.col > 0 && static_cast<size_t>(d.col - 1) < line_text.size() ? line_text.size() - static_cast<size_t>(d.col - 1) : 1;
        if (span > room) span = room;
        if (span > 40) span = 40;
        ss << "  " << std::string(width, ' ') << " | " << caret_padding(line_text, d.col) << "^" << std::string(span - 1, '~') << "\n";
        if (!after.empty()) ss << "  " << number(d.line + 1) << " | " << after << "\n";
    }
    if (!voiced.fix.empty() || !d.notes.empty() || !d.trail.empty()) ss << "\n";
    if (!voiced.fix.empty()) ss << "  fix: " << voiced.fix << "\n";
    for (const auto& n : d.notes) {
        ss << "  hint: " << voice_text(n) << "\n";
    }
    if (!d.trail.empty()) {
        ss << "  how it got here, newest first:\n";
        for (const auto& step : d.trail) ss << "    " << step << "\n";
    }
    ss << "\n";
    return scrub_text(ss.str());
}

std::string DiagBag::format(const std::string& source) const {
    std::stringstream ss;
    const std::vector<size_t> starts = diags_.empty() ? std::vector<size_t>{} : build_line_index(source);
    for (const auto& d : diags_) {
        ss << format_diag(d, source, &starts);
    }
    if (suppressed_ > 0) {
        ss << "\n  ... " << suppressed_ << " more diagnostic(s) not shown (only the first "
           << kMaxStoredErrors << " errors and " << kMaxStoredWarnings << " warnings get printed, fix those first)\n";
    }
    if (errors_ || warnings_) {
        ss << "  " << voice_footer(errors_, warnings_) << "\n";
    }
    return ss.str();
}

}

