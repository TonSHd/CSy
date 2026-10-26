#include "linker.hpp"
#include "resource.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <cctype>

namespace csy {
namespace fs = std::filesystem;
using SymbolMap = std::unordered_map<std::string, std::string>;

static std::string internal_name(size_t unit, const std::string& name) {
    std::string out = "__csy_u" + std::to_string(unit) + "_";
    static const char hex[] = "0123456789abcdef";
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_') out += (char)c;
        else { out += "_x"; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

static void gather_locals(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names,
                          bool nested = false) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->target) {
            if (s->expr->target->kind == ExprKind::Ident) names.insert(s->expr->target->name);
            else if (s->expr->target->kind == ExprKind::VectorLiteral)
                for (const auto& el : s->expr->target->elements) if (el && el->kind == ExprKind::Ident) names.insert(el->name);
        } else if (s->kind == StmtKind::FuncDecl) {
            if (nested) names.insert(s->func_name);
        } else if (s->kind == StmtKind::If) {
            gather_locals(s->then_body, names, nested);
            for (const auto& branch : s->elifs) gather_locals(branch.second, names, nested);
            gather_locals(s->else_body, names, nested);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            if (s->kind == StmtKind::For) names.insert(s->loop_var);
            gather_locals(s->body, names, nested);
        } else if (s->kind == StmtKind::Try) {
            names.insert(s->catch_var);
            gather_locals(s->body, names, nested);
            gather_locals(s->then_body, names, nested);
        }
    }
}

static void gather_module_globals(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names) {
    for (const auto& s : body) {
        if (!s || s->kind == StmtKind::FuncDecl) continue;
        if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->target) {
            if (s->expr->target->kind == ExprKind::Ident) names.insert(s->expr->target->name);
            else if (s->expr->target->kind == ExprKind::VectorLiteral)
                for (const auto& el : s->expr->target->elements) if (el && el->kind == ExprKind::Ident) names.insert(el->name);
        }
        if (s->kind == StmtKind::If) {
            gather_module_globals(s->then_body, names);
            for (const auto& branch : s->elifs) gather_module_globals(branch.second, names);
            gather_module_globals(s->else_body, names);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            gather_module_globals(s->body, names);
        } else if (s->kind == StmtKind::Try) {
            gather_module_globals(s->body, names);
            gather_module_globals(s->then_body, names);
        }
    }
}

struct Rewriter {
    const SymbolMap& own_symbols;
    const std::unordered_set<std::string>& own_module_globals;
    const std::unordered_map<std::string, const Stmt*>& aliases;
    const std::unordered_map<std::string, SymbolMap>& symbols_by_file;
    const std::string& file;
    std::unordered_map<std::string, std::string> target_cache{};
    const std::unordered_map<std::string, size_t>* arity = nullptr;
    std::vector<std::string>* warnings = nullptr;

    void expr(ExprPtr& e, const std::unordered_set<std::string>& locals) {
        if (!e) return;
        if (e->kind == ExprKind::Member && e->object && e->object->kind == ExprKind::Ident) {
            auto alias = aliases.find(e->object->name);
            if (alias != aliases.end()) {
                const Stmt& connect = *alias->second;
                if (std::find(connect.connect_exports.begin(), connect.connect_exports.end(), e->name) != connect.connect_exports.end()) {
                    auto cached = target_cache.find(e->object->name);
                    if (cached == target_cache.end()) {
                        fs::path target = fs::path(file).parent_path() / connect.connect_path;
                        std::error_code ec;
                        cached = target_cache.emplace(e->object->name,
                                                      fs::weakly_canonical(target, ec).string()).first;
                    }
                    const std::string& target_path = cached->second;
                    auto unit = symbols_by_file.find(target_path);
                    if (unit != symbols_by_file.end()) {
                        auto symbol = unit->second.find(e->name);
                        if (symbol != unit->second.end()) {
                            ExprPtr replacement = make_expr(ExprKind::Ident, e->tok);
                            replacement->name = symbol->second;
                            e = std::move(replacement);
                            return;
                        }
                    }
                }
            }
        }
        auto rename_ident = [&](ExprPtr& x) {
            if (x && x->kind == ExprKind::Ident && !locals.count(x->name)) {
                auto it = own_symbols.find(x->name);
                if (it != own_symbols.end()) x->name = it->second;
            }
        };
        switch (e->kind) {
            case ExprKind::Ident: rename_ident(e); break;
            case ExprKind::Binary: expr(e->left, locals); expr(e->right, locals); break;
            case ExprKind::Unary: expr(e->operand, locals); break;
            case ExprKind::Assign:
                expr(e->value, locals);
                if (e->target && e->target->kind == ExprKind::Ident) rename_ident(e->target);
                else expr(e->target, locals);
                break;
            case ExprKind::CompoundAssign:
                expr(e->target, locals); expr(e->value, locals); break;
            case ExprKind::Call: {
                std::string connected_name;
                if (e->callee && e->callee->kind == ExprKind::Member && e->callee->object &&
                    e->callee->object->kind == ExprKind::Ident && aliases.count(e->callee->object->name))
                    connected_name = e->callee->object->name + "::" + e->callee->name;
                expr(e->callee, locals);
                for (auto& a : e->args) expr(a, locals);
                if (!connected_name.empty() && warnings && arity && e->callee && e->callee->kind == ExprKind::Ident) {
                    auto it = arity->find(e->callee->name);
                    if (it != arity->end() && it->second != e->args.size()) {
                        warnings->push_back(e->tok.loc() + ": warning: '" + connected_name + "' takes " + std::to_string(it->second) +
                                            " argument(s), but this call passes " + std::to_string(e->args.size()));
                    }
                }
                break;
            }
            case ExprKind::Member: expr(e->object, locals); break;
            case ExprKind::Index: expr(e->object, locals); expr(e->index, locals); break;
            case ExprKind::VectorLiteral: for (auto& x : e->elements) expr(x, locals); break;
            case ExprKind::DictLiteral:
                for (auto& pair : e->dict_pairs) { expr(pair.first, locals); expr(pair.second, locals); }
                break;
            default: break;
        }
    }

    void body(std::vector<StmtPtr>& statements, std::unordered_set<std::string> locals) {
        for (auto& s : statements) {
            if (!s) continue;
            res::poll("linking");
            if (s->kind == StmtKind::FuncDecl) {
                auto nested_locals = locals;
                for (const auto& p : s->params) nested_locals.insert(p);
                gather_locals(s->body, nested_locals, true);
                for (const auto& name : own_module_globals) {
                    if (!locals.count(name) &&
                        std::find(s->params.begin(), s->params.end(), name) == s->params.end())
                        nested_locals.erase(name);
                }
                auto name = own_symbols.find(s->func_name);
                if (name != own_symbols.end() && !locals.count(s->func_name)) s->func_name = name->second;
                body(s->body, std::move(nested_locals));
                continue;
            }
            switch (s->kind) {
                case StmtKind::VarDecl: expr(s->expr, locals); break;
                case StmtKind::ExprStmt: expr(s->expr, locals); break;
                case StmtKind::If:
                    expr(s->cond, locals); body(s->then_body, locals);
                    for (auto& branch : s->elifs) { expr(branch.first, locals); body(branch.second, locals); }
                    body(s->else_body, locals); break;
                case StmtKind::While: expr(s->cond, locals); body(s->body, locals); break;
                case StmtKind::For: expr(s->iterable, locals); locals.insert(s->loop_var); body(s->body, locals); break;
                case StmtKind::Try: body(s->body, locals); locals.insert(s->catch_var); body(s->then_body, locals); break;
                case StmtKind::Block: body(s->body, locals); break;
                case StmtKind::Return: expr(s->ret_value, locals); break;
                default: break;
            }
        }
    }
};

bool namespace_project_symbols(std::vector<SourceUnit>& units, std::string& error, std::vector<std::string>* warnings) {
    std::unordered_map<std::string, SymbolMap> symbols_by_file;
    std::unordered_map<std::string, size_t> arity_by_symbol;
    for (size_t i = 0; i < units.size(); ++i) {
        auto& symbols = symbols_by_file[units[i].path];
        std::unordered_set<std::string> globals;
        gather_module_globals(units[i].parsed.module.body, globals);
        for (const auto& s : units[i].parsed.module.body) {
            std::string name;
            if (s->kind == StmtKind::FuncDecl) name = s->func_name;
            if (!name.empty()) {
                if (symbols.count(name)) { error = "duplicate top-level symbol '" + name + "' in '" + units[i].path + "'"; return false; }
                symbols[name] = internal_name(i, name);
                arity_by_symbol[internal_name(i, name)] = s->params.size();
            }
        }
        for (const auto& name : globals) {
            if (symbols.count(name)) { error = "duplicate top-level symbol '" + name + "' in '" + units[i].path + "'"; return false; }
            symbols[name] = internal_name(i, name);
        }
    }
    for (auto& unit : units) {
        std::unordered_map<std::string, const Stmt*> aliases;
        for (const auto& s : unit.parsed.module.body) if (s->kind == StmtKind::Connect) aliases[s->connect_alias] = s.get();
        std::unordered_set<std::string> globals;
        gather_module_globals(unit.parsed.module.body, globals);
        Rewriter rewriter{symbols_by_file[unit.path], globals, aliases, symbols_by_file, unit.path};
        rewriter.arity = &arity_by_symbol;
        rewriter.warnings = warnings;
        rewriter.body(unit.parsed.module.body, {});
    }
    return true;
}

std::vector<std::string> top_level_names(const Module& module, bool include_private) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string& name) {
        if (name.empty() || name[0] == '$') return;
        if (!include_private && name[0] == '_') return;
        if (seen.insert(name).second) names.push_back(name);
    };
    for (const auto& s : module.body) {
        if (!s) continue;
        if (s->kind == StmtKind::FuncDecl) add(s->func_name);
        else if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign &&
                 s->expr->target && s->expr->target->kind == ExprKind::Ident)
            add(s->expr->target->name);
    }
    return names;
}
}
