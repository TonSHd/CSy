#include "project.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <atomic>
#include <memory>
#include "resource.hpp"

namespace csy {
namespace fs = std::filesystem;
constexpr uintmax_t kMaxProjectConfigBytes = 1 * 1024 * 1024;
constexpr uintmax_t kMaxSourceBytes = 32 * 1024 * 1024;
constexpr uintmax_t kMaxProjectSourceBytes = 256 * 1024 * 1024;
constexpr size_t kMaxProjectFiles = 4096;
constexpr size_t kMaxConnectionDepth = 256;

static std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool unquote(const std::string& text, std::string& out) {
    const std::string s = trim(text);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    out = s.substr(1, s.size() - 2);
    return true;
}

static bool parse_sources_array(const std::string& text, std::vector<std::string>& out) {
    const std::string s = trim(text);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
    std::string item;
    bool quoted = false;
    for (size_t i = 1; i + 1 < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') { quoted = !quoted; item += c; }
        else if (c == ',' && !quoted) {
            std::string value;
            if (!unquote(item, value)) return false;
            out.push_back(value);
            if (out.size() > kMaxProjectFiles) return false;
            item.clear();
        } else item += c;
    }
    if (!trim(item).empty()) {
        std::string value;
        if (!unquote(item, value)) return false;
        out.push_back(value);
    }
    if (out.size() > kMaxProjectFiles) return false;
    return true;
}

bool resolve_project_inputs(const std::vector<std::string>& positional,
                            ProjectInputs& out, std::string& error) {
    if (positional.empty()) { error = "no input file or project was provided"; return false; }
    if (positional.size() == 1 && fs::path(positional.front()).extension() == ".csp") {
        const fs::path config_path = fs::absolute(positional.front());
        std::error_code size_error;
        const auto config_size = fs::file_size(config_path, size_error);
        if (size_error) {
            error = "cannot read project file '" + positional.front() + "'";
            return false;
        }
        if (config_size > kMaxProjectConfigBytes) {
            error = "project file exceeds the 1 MiB configuration limit";
            return false;
        }
        std::ifstream in(config_path);
        if (!in) { error = "cannot read project file '" + positional.front() + "'"; return false; }
        std::string line;
        bool has_entry = false;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) { error = "expected key = value in project file"; return false; }
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            std::string resolved;
            if (key == "entry") {
                if (!unquote(value, resolved)) { error = "project 'entry' must be a quoted path"; return false; }
                out.entry = (config_path.parent_path() / resolved).lexically_normal().string();
                has_entry = true;
            } else if (key == "sources") {
                std::vector<std::string> values;
                if (!parse_sources_array(value, values)) { error = "project 'sources' must be an array of quoted paths"; return false; }
                for (const auto& source : values)
                    out.sources.push_back((config_path.parent_path() / source).lexically_normal().string());
            } else if (key == "output") {
                if (!unquote(value, resolved)) { error = "project 'output' must be a quoted path"; return false; }
                out.output = (config_path.parent_path() / resolved).lexically_normal().string();
            } else {
                error = "unknown project setting '" + key + "'"; return false;
            }
        }
        if (!has_entry) { error = "project file needs an 'entry = \"...\"' setting"; return false; }
        return true;
    }
    out.entry = positional.front();
    for (size_t i = 1; i < positional.size(); ++i) out.sources.push_back(positional[i]);
    return true;
}

namespace {

struct Parsed {
    std::string error;
    uintmax_t bytes = 0;
    int failed_stage = 0;
    SourceUnit unit;
    std::vector<std::string> deps;
};

void read_lex_parse(const std::string& path, bool keep_tokens, Parsed& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        out.error = ec ? "cannot read source file '" + path + "'"
                       : "'" + path + "' is not a regular file";
        return;
    }
    const auto source_bytes = fs::file_size(path, ec);
    if (ec) { out.error = "cannot determine source size for '" + path + "'"; return; }
    if (source_bytes > kMaxSourceBytes) {
        out.error = "source file exceeds the 32 MiB safety limit: '" + path + "'";
        return;
    }
    out.bytes = source_bytes;
    std::ifstream in(path, std::ios::binary);
    if (!in) { out.error = "cannot read source file '" + path + "'"; return; }
    std::string source(static_cast<size_t>(source_bytes), '\0');
    if (source_bytes != 0) in.read(source.data(), static_cast<std::streamsize>(source_bytes));
    if (static_cast<uintmax_t>(in.gcount()) != source_bytes) {
        out.error = "source changed or could not be read completely: '" + path + "'";
        return;
    }
    char extra;
    if (in.get(extra)) { out.error = "source changed while being read: '" + path + "'"; return; }

    out.unit.path = path;
    out.unit.source = std::move(source);
    out.unit.lexed = lex(out.unit.source, path);
    if (!out.unit.lexed.ok) { out.failed_stage = 1; return; }
    res::check("lexing (finished)");
    out.unit.parsed = parse(out.unit.lexed.tokens, path, out.unit.source);
    res::check("parsing (finished)");
    if (!keep_tokens) std::vector<Token>().swap(out.unit.lexed.tokens);
    if (!out.unit.parsed.ok) { out.failed_stage = 2; return; }

    for (const auto& stmt : out.unit.parsed.module.body) {
        if (stmt->kind != StmtKind::Connect) continue;
        fs::path dependency = fs::path(stmt->connect_path);
        if (dependency.extension() != ".csy") continue;  // phase B reports it
        out.deps.push_back((fs::path(path).parent_path() / dependency).string());
    }
}

std::string canonical_path(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    fs::path real = fs::weakly_canonical(abs, ec);
    return (ec ? abs.lexically_normal() : real).string();
}

} // namespace

bool load_program_sources(const ProjectInputs& inputs,
                          std::vector<SourceUnit>& out,
                          std::string& error,
                          const std::function<bool()>& resource_check) {
    std::unordered_map<std::string, size_t> loaded;
    std::unordered_set<std::string> active;
    uintmax_t total_source_bytes = 0;

    std::unordered_map<std::string, std::unique_ptr<Parsed>> cache;
    {
        std::vector<std::string> frontier;
        for (const auto& source : inputs.sources) frontier.push_back(canonical_path(source));
        frontier.push_back(canonical_path(inputs.entry));
        while (!frontier.empty() && cache.size() < kMaxProjectFiles) {
            std::vector<std::string> wave;
            std::unordered_set<std::string> in_wave;
            for (auto& p : frontier)
                if (!cache.count(p) && in_wave.insert(p).second && cache.size() + wave.size() < kMaxProjectFiles)
                    wave.push_back(std::move(p));
            frontier.clear();
            if (wave.empty()) break;
            if (resource_check && !resource_check()) {
                error = "compiler memory budget reached while loading project sources";
                return false;
            }
            std::vector<std::unique_ptr<Parsed>> results(wave.size());
            std::atomic<uintmax_t> wave_bytes{total_source_bytes};
            uintmax_t largest = 0;
            for (const auto& file : wave) {
                std::error_code ec;
                const auto size = fs::file_size(file, ec);
                if (!ec && size > largest) largest = size;
            }
            const unsigned wave_threads = res::resolve_threads(
                inputs.threads, static_cast<uint64_t>(largest) * res::kEstimatedBytesPerSourceByte);
            res::parallel_for(wave.size(), wave_threads, [&](size_t i) {
                auto parsed = std::make_unique<Parsed>();
                std::error_code ec;
                const auto size = fs::file_size(wave[i], ec);
                if (!ec && wave_bytes.fetch_add(size) + size > kMaxProjectSourceBytes) {
                    parsed->error = "project source files exceed the 256 MiB combined safety limit";
                } else {
                    read_lex_parse(wave[i], inputs.keep_tokens, *parsed);
                }
                results[i] = std::move(parsed);
            });
            for (size_t i = 0; i < wave.size(); ++i) {
                for (const auto& dep : results[i]->deps) frontier.push_back(canonical_path(dep));
                cache.emplace(wave[i], std::move(results[i]));
            }
        }
    }

    auto canonical = [](const fs::path& p) { return canonical_path(p); };
    std::function<bool(const std::string&, bool, size_t, const Stmt*)> visit =
        [&](const std::string& requested, bool via_connection, size_t depth, const Stmt* from) {
        if (depth > kMaxConnectionDepth) {
            error = "project connection nesting exceeds the 256-file safety limit";
            return false;
        }
        if (resource_check && !resource_check()) {
            error = "compiler memory budget reached while loading project sources";
            return false;
        }
        const std::string path = canonical(requested);
        if (active.count(path)) {
            error = (from ? from->tok.loc() + ": " : std::string()) + "connection cycle detected: '" + path +
                    "' is already being loaded, so connecting it here would make the files depend on each other";
            return false;
        }
        if (loaded.count(path)) return true;
        if (out.size() >= kMaxProjectFiles) {
            error = "project contains more than 4096 source files";
            return false;
        }
        std::unique_ptr<Parsed> parsed;
        auto cached = cache.find(path);
        if (cached != cache.end()) {
            parsed = std::move(cached->second);
            cache.erase(cached);
        }
        if (!parsed) {
            parsed = std::make_unique<Parsed>();
            read_lex_parse(path, inputs.keep_tokens, *parsed);
        }
        if (!parsed->error.empty()) {
            error = parsed->error;
            if (from) {
                error = from->tok.loc() + ": " + error + " (named by #connect \"" + from->connect_path + "\")";
            }
            return false;
        }
        if (parsed->bytes > kMaxProjectSourceBytes - total_source_bytes) {
            error = "project source files exceed the 256 MiB combined safety limit";
            return false;
        }
        total_source_bytes += parsed->bytes;
        SourceUnit unit = std::move(parsed->unit);
        if (resource_check && !resource_check()) {
            error = "compiler memory budget reached while parsing '" + path + "'";
            return false;
        }
        if (parsed->failed_stage == 1) { error = "lexing failed in '" + path + "'"; out.push_back(std::move(unit)); return false; }
        if (parsed->failed_stage == 2) { error = "parsing failed in '" + path + "'"; out.push_back(std::move(unit)); return false; }
        parsed.reset();
        active.insert(path);
        for (const auto& stmt : unit.parsed.module.body) {
            if (stmt->kind != StmtKind::Connect) continue;
            fs::path dependency = fs::path(stmt->connect_path);
            if (dependency.extension() != ".csy") {
                error = stmt->tok.loc() + ": #connect expects a .csy source file, but got '" + stmt->connect_path + "'" +
                        (dependency.extension() == ".csh" ? " (.csh interface files are not used any more: connect the .csy file itself)" :
                         dependency.extension() == ".cso" ? " (a .cso is the build output and cannot be connected: connect the .csy it was built from)" : "");
                return false;
            }
            if (!visit((fs::path(path).parent_path() / dependency).string(), true, depth + 1, stmt.get())) return false;
        }
        active.erase(path);
        loaded[path] = out.size();
        out.push_back(std::move(unit));
        return true;
    };

    for (const auto& source : inputs.sources) if (!visit(source, false, 0, nullptr)) return false;
    if (!visit(inputs.entry, false, 0, nullptr)) return false;
    return true;
}

}
