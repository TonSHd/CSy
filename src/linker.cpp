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
        if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->kids().target) {
            if (s->expr->kids().target->kind == ExprKind::Ident) names.insert(s->expr->kids().target->name);
            else if (s->expr->kids().target->kind == ExprKind::VectorLiteral)
                for (const auto& el : s->expr->kids().target->kids().elements) if (el && el->kind == ExprKind::Ident) names.insert(el->name);
        } else if (s->kind == StmtKind::FuncDecl) {
            if (nested) names.insert(s->aux().func_name);
        } else if (s->kind == StmtKind::If) {
            gather_locals(s->aux().then_body, names, nested);
            for (const auto& branch : s->aux().elifs) gather_locals(branch.second, names, nested);
            gather_locals(s->aux().else_body, names, nested);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            if (s->kind == StmtKind::For) names.insert(s->aux().loop_var);
            gather_locals(s->body, names, nested);
        } else if (s->kind == StmtKind::Try) {
            names.insert(s->aux().catch_var);
            gather_locals(s->body, names, nested);
            gather_locals(s->aux().then_body, names, nested);
        }
    }
}

static void gather_module_globals(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& names) {
    for (const auto& s : body) {
        if (!s || s->kind == StmtKind::FuncDecl) continue;
        if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->kids().target) {
            if (s->expr->kids().target->kind == ExprKind::Ident) names.insert(s->expr->kids().target->name);
            else if (s->expr->kids().target->kind == ExprKind::VectorLiteral)
                for (const auto& el : s->expr->kids().target->kids().elements) if (el && el->kind == ExprKind::Ident) names.insert(el->name);
        }
        if (s->kind == StmtKind::If) {
            gather_module_globals(s->aux().then_body, names);
            for (const auto& branch : s->aux().elifs) gather_module_globals(branch.second, names);
            gather_module_globals(s->aux().else_body, names);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            gather_module_globals(s->body, names);
        } else if (s->kind == StmtKind::Try) {
            gather_module_globals(s->body, names);
            gather_module_globals(s->aux().then_body, names);
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
    const std::unordered_map<std::string, const Expr*>* using_aliases = nullptr;
    int alias_depth = 0;

    void expr(ExprPtr& e, const std::unordered_set<std::string>& locals) {
        if (!e) return;
        if (e->kind == ExprKind::Ident && using_aliases && alias_depth < 4 && !locals.count(e->name) &&
            !own_symbols.count(e->name)) {
            auto source = using_aliases->find(e->name);
            if (source != using_aliases->end()) {
                const Expr& target = *source->second;
                ExprPtr member = make_expr(ExprKind::Member, e->tok);
                member->kids_mut().object = make_expr(ExprKind::Ident, target.kids().object->tok);
                member->kids().object->name = target.kids().object->name;
                member->name = target.name;
                e = std::move(member);
                ++alias_depth;
                expr(e, locals);
                --alias_depth;
                return;
            }
        }
        if (e->kind == ExprKind::Member && e->kids().object && e->kids().object->kind == ExprKind::Ident) {
            auto alias = aliases.find(e->kids().object->name);
            if (alias != aliases.end()) {
                const Stmt& connect = *alias->second;
                if (std::find(connect.aux().connect_exports.begin(), connect.aux().connect_exports.end(), e->name) != connect.aux().connect_exports.end()) {
                    auto cached = target_cache.find(e->kids().object->name);
                    if (cached == target_cache.end()) {
                        fs::path target = fs::path(file).parent_path() / connect.aux().connect_path;
                        std::error_code ec;
                        cached = target_cache.emplace(e->kids().object->name,
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
            case ExprKind::Binary: expr(e->kids_mut().left, locals); expr(e->kids_mut().right, locals); break;
            case ExprKind::Unary: expr(e->kids_mut().operand, locals); break;
            case ExprKind::Assign:
                expr(e->kids_mut().value, locals);
                if (e->kids().target && e->kids().target->kind == ExprKind::Ident) rename_ident(e->kids_mut().target);
                else expr(e->kids_mut().target, locals);
                break;
            case ExprKind::CompoundAssign:
                expr(e->kids_mut().target, locals); expr(e->kids_mut().value, locals); break;
            case ExprKind::Call: {
                std::string connected_name;
                if (e->kids().callee && e->kids().callee->kind == ExprKind::Member && e->kids().callee->kids().object &&
                    e->kids().callee->kids().object->kind == ExprKind::Ident && aliases.count(e->kids().callee->kids().object->name))
                    connected_name = e->kids().callee->kids().object->name + "::" + e->kids().callee->name;
                expr(e->kids_mut().callee, locals);
                for (auto& a : e->kids_mut().args) expr(a, locals);
                if (!connected_name.empty() && warnings && arity && e->kids().callee && e->kids().callee->kind == ExprKind::Ident) {
                    auto it = arity->find(e->kids().callee->name);
                    if (it != arity->end() && it->second != e->kids().args.size()) {
                        warnings->push_back(e->tok.loc() + ": warning: '" + connected_name + "' takes " + std::to_string(it->second) +
                                            " argument(s), but this call passes " + std::to_string(e->kids().args.size()));
                    }
                }
                break;
            }
            case ExprKind::Member: expr(e->kids_mut().object, locals); break;
            case ExprKind::Index: expr(e->kids_mut().object, locals); expr(e->kids_mut().index, locals); break;
            case ExprKind::VectorLiteral: for (auto& x : e->kids_mut().elements) expr(x, locals); break;
            case ExprKind::DictLiteral:
                for (auto& pair : e->kids_mut().dict_pairs) { expr(pair.first, locals); expr(pair.second, locals); }
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
                for (const auto& p : s->aux().params) nested_locals.insert(p);
                gather_locals(s->body, nested_locals, true);
                for (const auto& name : own_module_globals) {
                    if (!locals.count(name) &&
                        std::find(s->aux().params.begin(), s->aux().params.end(), name) == s->aux().params.end())
                        nested_locals.erase(name);
                }
                auto name = own_symbols.find(s->aux().func_name);
                if (name != own_symbols.end() && !locals.count(s->aux().func_name)) s->aux_mut().func_name = name->second;
                body(s->body, std::move(nested_locals));
                continue;
            }
            switch (s->kind) {
                case StmtKind::VarDecl: expr(s->expr, locals); break;
                case StmtKind::ExprStmt: expr(s->expr, locals); break;
                case StmtKind::If:
                    expr(s->cond, locals); body(s->aux_mut().then_body, locals);
                    for (auto& branch : s->aux_mut().elifs) { expr(branch.first, locals); body(branch.second, locals); }
                    body(s->aux_mut().else_body, locals); break;
                case StmtKind::While: expr(s->cond, locals); body(s->body, locals); break;
                case StmtKind::For: expr(s->aux_mut().iterable, locals); locals.insert(s->aux().loop_var); body(s->body, locals); break;
                case StmtKind::Try: body(s->body, locals); locals.insert(s->aux().catch_var); body(s->aux_mut().then_body, locals); break;
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
            if (s->kind == StmtKind::FuncDecl) name = s->aux().func_name;
            if (!name.empty()) {
                if (symbols.count(name)) { error = "duplicate top-level symbol '" + name + "' in '" + units[i].path + "'"; return false; }
                symbols[name] = internal_name(i, name);
                arity_by_symbol[internal_name(i, name)] = s->aux().params.size();
            }
        }
        for (const auto& name : globals) {
            if (symbols.count(name)) { error = "duplicate top-level symbol '" + name + "' in '" + units[i].path + "'"; return false; }
            symbols[name] = internal_name(i, name);
        }
    }
    for (auto& unit : units) {
        std::unordered_map<std::string, const Stmt*> aliases;
        for (const auto& s : unit.parsed.module.body) if (s->kind == StmtKind::Connect) aliases[s->aux().connect_alias] = s.get();
        std::unordered_set<std::string> globals;
        gather_module_globals(unit.parsed.module.body, globals);
        Rewriter rewriter{symbols_by_file[unit.path], globals, aliases, symbols_by_file, unit.path};
        rewriter.arity = &arity_by_symbol;
        rewriter.warnings = warnings;
        std::unordered_set<std::string> module_aliases;
        for (const auto& s : unit.parsed.module.body)
            if (s && s->kind == StmtKind::Import) module_aliases.insert(s->aux().import_alias);
        std::unordered_map<std::string, const Expr*> using_map;
        for (const auto& s : unit.parsed.module.body) {
            if (!s || s->kind != StmtKind::Using || !s->aux().using_expr || s->aux().using_expr->kind != ExprKind::Member) continue;
            const Expr& target = *s->aux().using_expr;
            if (!target.kids().object || target.kids().object->kind != ExprKind::Ident || target.name == "out") continue;
            if (target.kids().object->name == s->aux().using_alias) continue;
            if (!module_aliases.count(target.kids().object->name) && !aliases.count(target.kids().object->name)) continue;
            using_map[s->aux().using_alias] = &target;
        }
        rewriter.using_aliases = &using_map;
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
        if (s->kind == StmtKind::FuncDecl) add(s->aux().func_name);
        else if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign &&
                 s->expr->kids().target && s->expr->kids().target->kind == ExprKind::Ident)
            add(s->expr->kids().target->name);
    }
    return names;
}
}

