#include "suggest.hpp"
#include <algorithm>
#include <cctype>
#include <numeric>
#include <sstream>

namespace csy {

static int edit_distance(const std::string& a, const std::string& b) {
    int n = (int)a.size();
    int m = (int)b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j-1] + 1, prev[j-1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

std::vector<std::string> suggest(const std::string& target,
                                 const std::vector<std::string>& candidates,
                                 int max_distance) {
    std::vector<std::pair<int, std::string>> scored;
    const int allowed = std::min(max_distance, std::max(1, static_cast<int>(target.size()) / 3));
    auto lowered = [](std::string text) {
        for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    };
    const std::string target_lower = lowered(target);
    for (const auto& c : candidates) {
        if (c.empty()) continue;
        const bool same_ignoring_case = lowered(c) == target_lower && c != target;
        if (!same_ignoring_case && (target.size() < 3 || c.size() < 3)) continue;
        int d = same_ignoring_case ? 0 : edit_distance(target, c);
        if (!same_ignoring_case &&
            (c.find(target) != std::string::npos || target.find(c) != std::string::npos)) {
            d = std::max(0, d - 1);
        }
        if (d <= allowed) {
            scored.emplace_back(d, c);
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (const auto& s : scored) out.push_back(s.second);
    return out;
}

std::string suggest_one(const std::string& target,
                        const std::vector<std::string>& candidates,
                        int max_distance) {
    auto s = suggest(target, candidates, max_distance);
    if (s.empty()) return "";
    return s[0];
}

std::string did_you_mean(const std::string& target,
                         const std::vector<std::string>& candidates) {
    auto s = suggest(target, candidates, 3);
    if (s.empty()) return "";
    if (s.size() == 1) {
        return "did you mean '" + s[0] + "'?";
    }
    std::stringstream ss;
    ss << "did you mean one of: ";
    for (size_t i = 0; i < s.size() && i < 3; i++) {
        if (i) ss << ", ";
        ss << "'" << s[i] << "'";
    }
    ss << "?";
    return ss.str();
}

}

