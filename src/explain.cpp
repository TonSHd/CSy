#include "explain.hpp"
#include "linker.hpp"
#include "suggest.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

namespace csy {
namespace {
namespace fs = std::filesystem;

struct Decl {
    std::string name;
    const char* kind;
    int line;
};

std::vector<Decl> declarations(const Module& module) {
    std::vector<Decl> out;
    std::unordered_set<std::string> seen;
    for (const auto& s : module.body) {
        if (!s) continue;
        if (s->kind == StmtKind::FuncDecl) {
            if (!s->func_name.empty() && s->func_name[0] != '$' && seen.insert(s->func_name).second)
                out.push_back({s->func_name, "func", s->tok.line});
        } else if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->target &&
                   s->expr->target->kind == ExprKind::Ident) {
            const std::string& name = s->expr->target->name;
            if (seen.insert(name).second) out.push_back({name, "var", s->tok.line});
        }
    }
    return out;
}

std::string canon_path(const fs::path& p) {
    std::error_code ec;
    fs::path real = fs::weakly_canonical(p, ec);
    return (ec ? p.lexically_normal() : real).string();
}

struct Index {
    const std::vector<SourceUnit>& units;
    std::vector<std::vector<Decl>> decls;
    std::unordered_map<std::string, size_t> by_path;

    explicit Index(const std::vector<SourceUnit>& u) : units(u) {
        for (size_t i = 0; i < units.size(); ++i) {
            decls.push_back(declarations(units[i].parsed.module));
            by_path[units[i].path] = i;
        }
    }

    const Decl* find_decl(size_t unit, const std::string& name) const {
        for (const auto& d : decls[unit]) if (d.name == name) return &d;
        return nullptr;
    }

    bool connection(size_t unit, const std::string& alias, size_t& target, std::string& written) const {
        for (const auto& s : units[unit].parsed.module.body) {
            if (!s || s->kind != StmtKind::Connect || s->connect_alias != alias) continue;
            auto it = by_path.find(canon_path(fs::path(units[unit].path).parent_path() / s->connect_path));
            if (it == by_path.end()) return false;
            target = it->second;
            written = s->connect_path;
            return true;
        }
        return false;
    }

    std::vector<const Stmt*> explains_of(size_t unit, const std::string& name) const {
        std::vector<const Stmt*> out;
        for (const auto& e : units[unit].parsed.module.explains) if (e->explain_name == name) out.push_back(e.get());
        return out;
    }
};

struct Resolved {
    bool found = false;
    size_t unit = 0;
    const Decl* decl = nullptr;
    std::string via;
};

Resolved resolve(const Index& idx, size_t unit, const std::string& name) {
    Resolved r;
    const size_t cut = name.find("::");
    if (cut == std::string::npos) {
        r.decl = idx.find_decl(unit, name);
        r.found = r.decl != nullptr;
        r.unit = unit;
        return r;
    }
    const std::string alias = name.substr(0, cut), member = name.substr(cut + 2);
    size_t target = 0;
    std::string written;
    if (!idx.connection(unit, alias, target, written)) return r;
    if (!member.empty() && member[0] == '_') return r;
    r.decl = idx.find_decl(target, member);
    r.found = r.decl != nullptr;
    r.unit = target;
    r.via = written;
    return r;
}

std::string describe(const Resolved& r) {
    std::string s = std::string("(") + r.decl->kind + "; ";
    if (!r.via.empty()) s += r.via + " ";
    return s + "line " + std::to_string(r.decl->line) + ")";
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

std::string shown_path(const Index& idx, size_t unit, const fs::path& base) {
    std::error_code ec;
    fs::path rel = fs::relative(idx.units[unit].path, base, ec);
    return ec || rel.empty() ? idx.units[unit].path : rel.string();
}

void print_name(const Index& idx, size_t unit, const std::string& name, const Decl* decl, const std::string& via) {
    const auto explains = idx.explains_of(unit, name);
    std::string where = decl ? std::string("(") + decl->kind + "; " + (via.empty() ? "" : via + " ") + "line " + std::to_string(decl->line) + ")"
                             : "(not declared in this file)";
    if (explains.empty()) {
        std::cout << name << " " << where << "\nno #explain has been written for it\n";
        return;
    }
    std::cout << quoted(explains[0]->explain_text) << " " << where << "\n";
    if (!explains[0]->explain_refs.empty()) {
        std::cout << "\nREFER: ";
        bool first = true;
        for (const auto& ref : explains[0]->explain_refs) {
            if (!first) std::cout << ", ";
            first = false;
            const Resolved r = resolve(idx, unit, ref);
            if (r.found) std::cout << ref << describe(r);
            else std::cout << ref << "(not found)";
        }
        std::cout << "\n";
    }
    if (explains.size() > 1) {
        std::cout << "\nnote: '" << name << "' is explained " << explains.size() << " times; the first one is shown (lines";
        for (const Stmt* e : explains) std::cout << " " << e->tok.line;
        std::cout << ")\n";
    }
}

void print_usage(std::ostream& out) {
    out << "usage: csyc explain [NAME] [--path FILE.csy | project.csp]\n"
           "  NAME           a function or variable, or namespace::name for something in a connected file;\n"
           "                 leave it out to list every name of the file with its explanation\n"
           "  --path PATH    the .csy file or the project.csp whose files are searched (default: project.csp here)\n";
}

}

int explain_command(int argc, char** argv) {
    std::string name, path;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(std::cout); return 0; }
        if (a == "--path") {
            if (i + 1 >= argc) { std::cerr << "error: --path needs a file\n"; print_usage(std::cerr); return 1; }
            path = argv[++i];
        } else if (a.rfind("--path=", 0) == 0) {
            path = a.substr(7);
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n";
            print_usage(std::cerr);
            return 1;
        } else if (name.empty()) {
            name = a;
        } else {
            std::cerr << "error: only one name can be explained at a time\n";
            print_usage(std::cerr);
            return 1;
        }
    }
    if (path.empty()) {
        std::error_code ec;
        if (!fs::exists("project.csp", ec)) {
            std::cerr << "error: no --path was given and there is no project.csp in this folder\n";
            print_usage(std::cerr);
            return 1;
        }
        path = "project.csp";
    }
    const bool whole_project = fs::path(path).extension() == ".csp";

    ProjectInputs inputs;
    std::string error;
    if (!resolve_project_inputs({path}, inputs, error)) { std::cerr << "error: " << error << "\n"; return 1; }
    inputs.threads = 1;
    std::vector<SourceUnit> units;
    if (!load_program_sources(inputs, units, error)) {
        std::cerr << "error: " << error << "\n";
        for (const auto& unit : units)
            if (!unit.parsed.diags.items().empty()) std::cerr << unit.parsed.diags.format(unit.source);
        return 1;
    }
    for (const auto& unit : units)
        if (!unit.parsed.ok) {
            std::cerr << unit.parsed.diags.format(unit.source) << "error: " << unit.path << " has errors, so it cannot be explained yet\n";
            return 1;
        }
    const Index idx(units);
    const size_t entry = units.size() - 1;
    const fs::path base = fs::path(units[entry].path).parent_path();

    if (name.empty()) {
        bool printed = false;
        for (size_t u = 0; u < units.size(); ++u) {
            if (!whole_project && u != entry) continue;
            if (whole_project) std::cout << (printed ? "\n" : "") << "== " << shown_path(idx, u, base) << " ==\n";
            printed = true;
            std::unordered_set<std::string> listed;
            for (const auto& d : idx.decls[u]) {
                listed.insert(d.name);
                const auto ex = idx.explains_of(u, d.name);
                std::cout << d.name << " (" << d.kind << "; line " << d.line << "): "
                          << (ex.empty() ? std::string("no #explain") : quoted(ex[0]->explain_text)) << "\n";
            }
            for (const auto& e : units[u].parsed.module.explains)
                if (!listed.count(e->explain_name) && e->explain_name.find("::") == std::string::npos) {
                    listed.insert(e->explain_name);
                    std::cout << e->explain_name << " (not declared in this file): " << quoted(e->explain_text) << "\n";
                }
            if (idx.decls[u].empty() && units[u].parsed.module.explains.empty()) std::cout << "(no functions or variables)\n";
        }
        return 0;
    }

    if (name.find("::") != std::string::npos) {
        const Resolved r = resolve(idx, entry, name);
        if (!r.found) {
            const size_t cut = name.find("::");
            size_t target = 0;
            std::string written;
            if (!idx.connection(entry, name.substr(0, cut), target, written)) {
                std::cerr << "error: '" << name.substr(0, cut) << "' is not a #connect namespace in " << shown_path(idx, entry, base) << "\n";
            } else {
                std::cerr << "error: '" << name.substr(cut + 2) << "' is not shared by " << written << "\n";
                std::vector<std::string> names;
                for (const auto& d : idx.decls[target]) if (d.name[0] != '_') names.push_back(d.name);
                const std::string hint = did_you_mean(name.substr(cut + 2), names);
                if (!hint.empty()) std::cerr << hint << "\n";
            }
            return 1;
        }
        print_name(idx, r.unit, r.decl->name, r.decl, r.via);
        return 0;
    }

    bool any = false;
    for (size_t u = 0; u < units.size(); ++u) {
        if (!whole_project && u != entry) continue;
        const Decl* d = idx.find_decl(u, name);
        const auto ex = idx.explains_of(u, name);
        if (!d && ex.empty()) continue;
        if (any) std::cout << "\n";
        if (whole_project) std::cout << "== " << shown_path(idx, u, base) << " ==\n";
        print_name(idx, u, name, d, "");
        any = true;
    }
    if (!any) {
        std::vector<std::string> names;
        for (size_t u = 0; u < units.size(); ++u)
            if (whole_project || u == entry) for (const auto& d : idx.decls[u]) names.push_back(d.name);
        std::cerr << "error: no function or variable called '" << name << "' in " << (whole_project ? "this project" : shown_path(idx, entry, base)) << "\n";
        const std::string hint = did_you_mean(name, names);
        if (!hint.empty()) std::cerr << hint << "\n";
        return 1;
    }
    return 0;
}

std::vector<std::string> explain_warnings(const std::vector<SourceUnit>& units) {
    std::vector<std::string> out;
    const Index idx(units);
    for (size_t u = 0; u < units.size(); ++u) {
        std::unordered_map<std::string, int> first_line;
        for (const auto& e : units[u].parsed.module.explains) {
            const std::string where = e->tok.loc() + ": warning: ";
            auto seen = first_line.emplace(e->explain_name, e->tok.line);
            if (!seen.second)
                out.push_back(where + "'" + e->explain_name + "' is already explained on line " + std::to_string(seen.first->second));
            if (!resolve(idx, u, e->explain_name).found)
                out.push_back(where + "#explain: '" + e->explain_name + "' is not a function or variable at the top level of this file");
            for (const auto& ref : e->explain_refs)
                if (!resolve(idx, u, ref).found)
                    out.push_back(where + "#explain " + e->explain_name + ": ref '" + ref + "' is not a function or variable it can reach (use namespace::name for connected files)");
        }
    }
    return out;
}

}

