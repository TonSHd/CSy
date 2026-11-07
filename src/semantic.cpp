#include "semantic.hpp"
#include "resource.hpp"
#include "suggest.hpp"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>

namespace csy {

class Analyzer {
public:
    Analyzer(const std::string& source, const std::vector<std::string>& builtin_names)
        : source_(source), builtin_names_(builtin_names),
          builtin_set_(builtin_names.begin(), builtin_names.end()) {}

    SemanticResult run(const Module& mod) {
        std::unordered_map<std::string, const Stmt*> namespace_aliases;
        for (const auto& s : mod.body) {
            if (s->kind != StmtKind::Connect && s->kind != StmtKind::Import) continue;
            const std::string& alias = s->kind == StmtKind::Connect ? s->aux().connect_alias : s->aux().import_alias;
            auto inserted = namespace_aliases.emplace(alias, s.get());
            if (!inserted.second) {
                error(s->tok, "namespace '" + alias + "' is already declared in this file");
            }
        }
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::Connect) {
                auto inserted = connected_exports_.emplace(s->aux().connect_alias,
                    std::unordered_set<std::string>(s->aux().connect_exports.begin(), s->aux().connect_exports.end()));
                if (!inserted.second) {
                    error(s->tok, "connection namespace '" + s->aux().connect_alias + "' is declared more than once");
                }
            }
            if (s->kind == StmtKind::FuncDecl) {
                bool mangled = !s->aux().func_name.empty() && s->aux().func_name[0] == '$';
                if (!mangled) {
                    auto it = declared_kind_.find(s->aux().func_name);
                    if (it != declared_kind_.end()) {
                        warn(s->tok, "'" + s->aux().func_name + "' is already defined as a " +
                             it->second + " - this redefines it, the earlier one is gone");
                    }
                    declared_kind_[s->aux().func_name] = "function";
                }
                func_names_.push_back(s->aux().func_name);
                func_name_set_.insert(s->aux().func_name);
                func_arity_[s->aux().func_name] = (int)s->aux().params.size();
            }
        }
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign) {
                auto& target = s->expr->kids().target;
                if (target && target->kind == ExprKind::Ident) {
                    auto it = declared_kind_.find(target->name);
                    if (it != declared_kind_.end() && it->second == "function") {
                        warn(s->tok, "'" + target->name + "' is already defined as a function - " +
                             "this assignment won't be visible later, reading '" + target->name +
                             "' will still resolve to the function, not this value");
                    } else if (it == declared_kind_.end()) {
                        declared_kind_[target->name] = "variable";
                    }
                    module_globals_.insert(target->name);
                } else if (target && target->kind == ExprKind::VectorLiteral) {
                    for (const auto& el : target->kids().elements) {
                        if (el && el->kind == ExprKind::Ident) {
                            module_globals_.insert(el->name);
                        }
                    }
                }
            }
        }
        Scope module_scope;
        module_scope.names = module_globals_;
        visit_block(mod.body, module_scope);
        result_.ok = !result_.diags.has_errors();
        return result_;
    }

    bool is_gpu_module(const std::string& module) {
        return module == "nvidia" || module == "cuda";
    }

    bool is_tui_module(const std::string& module) {
        return module == "tui";
    }

    bool is_ai_module(const std::string& module) {
        return module == "ai";
    }

    bool is_path_module(const std::string& module) {
        return module == "path";
    }

    bool is_fs_module(const std::string& module) {
        return module == "fs";
    }

    bool is_process_module(const std::string& module) {
        return module == "process";
    }

    bool is_shell_module(const std::string& module) {
        return module == "shell";
    }

    bool is_builtin_name(const std::string& n) {
        return builtin_set_.count(n) > 0;
    }

    bool module_has_member(const std::string& module, const std::string& name) {
        if (module == "basic") return is_builtin_name(name);
        if (is_gpu_module(module)) return is_builtin_name("nv_" + name);
        if (is_tui_module(module)) return is_builtin_name("tui_" + name);
        if (is_ai_module(module)) return is_builtin_name("ai_" + name);
        if (is_path_module(module)) return is_builtin_name("path_" + name);
        if (is_fs_module(module)) return is_builtin_name("fs_" + name);
        if (is_process_module(module)) return is_builtin_name("process_" + name);
        if (is_shell_module(module)) return is_builtin_name("shell_" + name);
        return false;
    }

    std::vector<std::string> module_member_candidates(const std::string& module) {
        std::vector<std::string> out;
        for (const auto& n : builtin_names_) {
            bool is_nv = n.rfind("nv_", 0) == 0;
            bool is_tui = n.rfind("tui_", 0) == 0;
            bool is_ai = n.rfind("ai_", 0) == 0;
            bool is_path = n.rfind("path_", 0) == 0;
            bool is_fs = n.rfind("fs_", 0) == 0;
            bool is_process = n.rfind("process_", 0) == 0;
            bool is_shell = n.rfind("shell_", 0) == 0;
            if (is_gpu_module(module) && is_nv) out.push_back(n.substr(3));
            else if (is_tui_module(module) && is_tui) out.push_back(n.substr(4));
            else if (is_ai_module(module) && is_ai) out.push_back(n.substr(3));
            else if (is_path_module(module) && is_path) out.push_back(n.substr(5));
            else if (is_fs_module(module) && is_fs) out.push_back(n.substr(3));
            else if (is_process_module(module) && is_process) out.push_back(n.substr(8));
            else if (is_shell_module(module) && is_shell) out.push_back(n.substr(6));
            else if (module == "basic" && !is_nv && !is_tui && !is_ai && !is_path && !is_fs &&
                     !is_process && !is_shell) out.push_back(n);
        }
        return out;
    }

private:
    SemanticResult result_;
    const std::string& source_;
    std::vector<std::string> builtin_names_;
    std::unordered_set<std::string> builtin_set_;
    std::vector<std::string> func_names_;
    std::unordered_map<std::string, int> func_arity_;
    std::unordered_map<std::string, std::string> declared_kind_;
    std::unordered_set<std::string> module_globals_;
    std::vector<std::string> import_aliases_;
    std::unordered_map<std::string, std::string> alias_to_module_;
    std::unordered_map<std::string, std::unordered_set<std::string>> connected_exports_;
    std::vector<std::pair<std::string, std::string>> using_aliases_;
    int loop_depth_ = 0;
    int function_depth_ = 0;
    std::unordered_set<std::string> top_assigned_;
    std::vector<std::string> active_loop_vars_;

    struct Scope {
        std::unordered_set<std::string> names;
        const Scope* parent = nullptr;
        bool count(const std::string& n) const {
            for (const Scope* sc = this; sc; sc = sc->parent) if (sc->names.count(n)) return true;
            return false;
        }
        void insert(const std::string& n) { names.insert(n); }
    };
    std::unordered_set<std::string> func_name_set_, import_alias_set_, using_alias_set_;

    void error(const Token& t, const std::string& msg) {
        result_.diags.error(t.file(), t.line, t.col, msg, t.text.empty() ? 1 : static_cast<int>(t.text.size()));
    }

    void visit_block(const std::vector<StmtPtr>& body, Scope& scope) {
        bool terminated = false, warned = false;
        const char* by = "";
        for (const auto& st : body) {
            if (!st) continue;
            if (terminated && !warned) {
                warn(st->tok, std::string("unreachable code: this follows '") + by + "' in the same block and can never run");
                warned = true;
            }
            visit_stmt(st, scope);
            if (st->kind == StmtKind::Return) { terminated = true; by = "rt"; }
            else if (st->kind == StmtKind::Break) { terminated = true; by = "stop"; }
            else if (st->kind == StmtKind::Continue) { terminated = true; by = "skip"; }
        }
    }

    static bool is_literal(const ExprPtr& e) {
        return e && (e->kind == ExprKind::Int || e->kind == ExprKind::Float || e->kind == ExprKind::Bool);
    }
    static bool literal_truthy(const ExprPtr& e) {
        if (e->kind == ExprKind::Int) return e->int_val != 0;
        if (e->kind == ExprKind::Float) return e->float_val != 0.0;
        return e->bool_val;
    }

    void warn(const Token& t, const std::string& msg) {
        result_.diags.warn(t.file(), t.line, t.col, msg, t.text.empty() ? 1 : static_cast<int>(t.text.size()));
    }

    std::vector<std::string> all_known_names(const Scope& local) {
        std::vector<std::string> all;
        for (const Scope* sc = &local; sc; sc = sc->parent)
            for (const auto& n : sc->names) all.push_back(n);
        for (const auto& n : func_names_) all.push_back(n);
        for (const auto& n : import_aliases_) all.push_back(n);
        for (const auto& p : using_aliases_) all.push_back(p.first);
        for (const auto& n : builtin_names_) all.push_back(n);
        return all;
    }

    void note_did_you_mean(const Token& t, const std::string& target, const Scope& local) {
        std::vector<std::string> owners;
        for (const auto& kv : connected_exports_)
            if (kv.second.count(target)) owners.push_back(kv.first);
        if (!owners.empty()) {
            std::sort(owners.begin(), owners.end());
            std::string msg = "'" + target + "' comes from a #connect: write ";
            for (size_t i = 0; i < owners.size(); i++) msg += (i ? " or " : "") + owners[i] + "::" + target;
            msg += " (names from #connect are always used through their namespace)";
            result_.diags.note(msg);
            return;
        }
        auto candidates = all_known_names(local);
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        auto msg = did_you_mean(target, candidates);
        if (!msg.empty()) result_.diags.note(msg);
    }

    bool is_namespace_name(const std::string& name) const {
        return (import_alias_set_.count(name) != 0 || connected_exports_.count(name) != 0) &&
               module_globals_.count(name) == 0;
    }

    void note_top_assignment(const std::string& name) {
        if (function_depth_ == 0) top_assigned_.insert(name);
    }

    void check_assign_target(const ExprPtr& target, const Token& at) {
        if (!target) return;
        switch (target->kind) {
            case ExprKind::Ident:
            case ExprKind::Index:
            case ExprKind::VectorLiteral:
                break;
            case ExprKind::Member: {
                const auto& obj = target->kids().object;
                if (obj && obj->kind == ExprKind::Ident && is_namespace_name(obj->name)) {
                    error(at, "cannot assign to '" + obj->name + "::" + target->name +
                              "': names that belong to a module or a #connect are read-only");
                }
                break;
            }
            case ExprKind::Bool:
            case ExprKind::None:
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::String:
                error(at, "cannot assign to a literal value; the left side of '=' must be a variable, an element or a field");
                break;
            default:
                error(at, "the left side of '=' must be a variable, an element or a field");
                break;
        }
    }

    void visit_stmt(const StmtPtr& s, Scope& local) {
        res::poll("semantic analysis");
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Import: {
                import_aliases_.push_back(s->aux().import_alias);
                import_alias_set_.insert(s->aux().import_alias);
                alias_to_module_[s->aux().import_alias] = s->aux().import_module;
                result_.import_aliases.push_back(s->aux().import_alias);
                if (is_gpu_module(s->aux().import_module)) {
                    warn(s->tok,
                         "the 'nvidia' and 'cuda' imports are deprecated and remain supported for compatibility; for MLP training, use '#inc ai as ai;' and ai::mlp_train_gpu. Existing nv:: hardware and matmul functions still work");
                }
                if (s->aux().import_module != "basic" && !is_gpu_module(s->aux().import_module) &&
                    !is_tui_module(s->aux().import_module) && !is_ai_module(s->aux().import_module) &&
                    !is_path_module(s->aux().import_module) && !is_fs_module(s->aux().import_module) &&
                    !is_process_module(s->aux().import_module) && !is_shell_module(s->aux().import_module)) {
                    warn(s->tok, "unknown module '" + s->aux().import_module +
                         "', only 'basic', 'nvidia', 'cuda', 'tui', 'ai', 'path', 'fs', 'process', and 'shell' are supported. The alias '" +
                         s->aux().import_alias + "' will be created but it has no members.");
                }
                break;
            }
            case StmtKind::Connect:
                import_aliases_.push_back(s->aux().connect_alias);
                import_alias_set_.insert(s->aux().connect_alias);
                local.insert(s->aux().connect_alias);
                break;
            case StmtKind::Explain:
            case StmtKind::DirectiveAlias:
                break;
            case StmtKind::Using: {
                if (s->aux().using_expr && s->aux().using_expr->kind == ExprKind::Member) {
                    const auto& obj = s->aux().using_expr->kids().object;
                    if (obj && obj->kind == ExprKind::Ident) {
                        auto module = alias_to_module_.find(obj->name);
                        if (module != alias_to_module_.end() &&
                            module->second == "basic" && s->aux().using_expr->name == "matmul") {
                            warn(s->tok, "using basic.matmul is deprecated for performance-sensitive AI workloads; it remains supported, but use ai::matmul with packed f64_array buffers for new code");
                        } else {
                            visit_expr(s->aux().using_expr, local);
                        }
                        using_aliases_.push_back({s->aux().using_alias, s->aux().using_expr->name});
                using_alias_set_.insert(s->aux().using_alias);
                        result_.using_aliases.push_back({s->aux().using_alias, s->aux().using_expr->name});
                        local.insert(s->aux().using_alias);
                    } else {
                        warn(s->tok, "unsupported 'using' target - only `using <module>.<member> as <name>` is supported in v1");
                    }
                } else {
                    warn(s->tok, "unsupported 'using' target - only `using <module>.<member> as <name>` is supported in v1");
                }
                break;
            }
            case StmtKind::VarDecl: {
                if (s->expr && s->expr->kind == ExprKind::Assign) {
                    visit_expr(s->expr->kids().value, local);
                    auto& target = s->expr->kids().target;
                    check_assign_target(target, s->tok);
                    if (target && target->kind != ExprKind::Ident && target->kind != ExprKind::VectorLiteral)
                        visit_expr(target, local);
                    if (target && target->kind == ExprKind::Ident) {
                        note_top_assignment(target->name);
                        if (s->expr->kids().value && s->expr->kids().value->kind == ExprKind::Ident &&
                            s->expr->kids().value->name == target->name) {
                            warn(s->tok, "'" + target->name + " = " + target->name + "' assigns a variable to itself and does nothing");
                        }
                        if (!local.count(target->name) && builtin_set_.count(target->name) &&
                            !func_name_set_.count(target->name)) {
                            warn(s->tok, "variable '" + target->name + "' hides the built-in function of the same name; calls to " +
                                         target->name + "(...) after this point will not reach the built-in");
                        }
                        if (std::find(active_loop_vars_.begin(), active_loop_vars_.end(), target->name) != active_loop_vars_.end()) {
                            warn(s->tok, "assigning to loop variable '" + target->name +
                                         "' does not change how many times the loop runs (it is reset on every round)");
                        }
                        local.insert(target->name);
                    } else if (target && target->kind == ExprKind::VectorLiteral) {
                        for (const auto& el : target->kids().elements) {
                            if (el && el->kind == ExprKind::Ident) { local.insert(el->name); note_top_assignment(el->name); }
                        }
                    }
                } else {
                    visit_expr(s->expr, local);
                }
                break;
            }
            case StmtKind::ExprStmt:
                visit_expr(s->expr, local);
                if (s->expr && s->expr->kind == ExprKind::Binary) {
                    const Expr* left_most = s->expr.get();
                    bool only_comparisons = true;
                    while (left_most && left_most->kind == ExprKind::Binary) {
                        const TokenType o = left_most->op;
                        if (o != TokenType::GT && o != TokenType::LT && o != TokenType::GE && o != TokenType::LE &&
                            o != TokenType::EQ && o != TokenType::NEQ) { only_comparisons = false; break; }
                        left_most = left_most->kids().left.get();
                    }
                    if (only_comparisons && left_most && left_most->kind == ExprKind::Ident && left_most->name == "out" &&
                        !local.count("out") && func_name_set_.count("out") == 0 && using_alias_set_.count("out") == 0) {
                        warn(left_most->tok, "'out' on its own is just the print function, so this line compares it and prints nothing - "
                                             "import the module ('#inc basic as bc;') and write bc::out>..., or alias it with 'using bc::out as o;'");
                    }
                }
                break;
            case StmtKind::FuncDecl: {
                local.insert(s->aux().func_name);
                Scope fn_scope;
                fn_scope.parent = &local;
                {
                    std::unordered_set<std::string> seen_params;
                    for (const auto& p : s->aux().params) {
                        if (!seen_params.insert(p).second)
                            error(s->tok, "the parameter '" + p + "' is declared twice in func " + s->aux().func_name);
                    }
                }
                for (const auto& p : s->aux().params) fn_scope.insert(p);
                const int saved_loop_depth = loop_depth_;
                loop_depth_ = 0;
                std::vector<std::string> saved_loop_vars;
                saved_loop_vars.swap(active_loop_vars_);
                function_depth_++;
                visit_block(s->body, fn_scope);
                function_depth_--;
                loop_depth_ = saved_loop_depth;
                active_loop_vars_.swap(saved_loop_vars);
                break;
            }
            case StmtKind::If: {
                if (is_literal(s->cond)) {
                    warn(s->tok, std::string("this 'if' condition is a constant that is always ") +
                                 (literal_truthy(s->cond) ? "true" : "false") + " - one branch can never run");
                }
                visit_expr(s->cond, local);
                visit_block(s->aux().then_body, local);
                for (const auto& el : s->aux().elifs) {
                    visit_expr(el.first, local);
                    visit_block(el.second, local);
                }
                if (!s->aux().else_body.empty()) {
                    visit_block(s->aux().else_body, local);
                }
                break;
            }
            case StmtKind::While: {
                if (is_literal(s->cond) && !literal_truthy(s->cond)) {
                    warn(s->tok, "this 'while' condition is always false - the loop body never runs");
                }
                visit_expr(s->cond, local);
                loop_depth_++;
                visit_block(s->body, local);
                loop_depth_--;
                break;
            }
            case StmtKind::For: {
                visit_expr(s->aux().iterable, local);
                local.insert(s->aux().loop_var);
                note_top_assignment(s->aux().loop_var);
                loop_depth_++;
                active_loop_vars_.push_back(s->aux().loop_var);
                visit_block(s->body, local);
                active_loop_vars_.pop_back();
                loop_depth_--;
                break;
            }
            case StmtKind::Break:
                if (loop_depth_ == 0) {
                    warn(s->tok, "'stop' used outside of a loop - has no effect here");
                }
                break;
            case StmtKind::Continue:
                if (loop_depth_ == 0) {
                    warn(s->tok, "'skip' used outside of a loop - has no effect here");
                }
                break;
            case StmtKind::Return:
                if (s->ret_value) visit_expr(s->ret_value, local);
                break;
            case StmtKind::Try: {
                visit_block(s->body, local);
                Scope catch_scope;
                catch_scope.parent = &local;
                catch_scope.insert(s->aux().catch_var);
                note_top_assignment(s->aux().catch_var);
                visit_block(s->aux().then_body, catch_scope);
                break;
            }
            case StmtKind::Block:
                visit_block(s->body, local);
                break;
        }
    }

    bool local_is_variable(const Scope& local, const std::string& name) const {
        return local.count(name) && module_globals_.count(name) != 0;
    }

    void visit_expr(const ExprPtr& e, const Scope& local) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::String:
            case ExprKind::Bool:
            case ExprKind::None:
                break;
            case ExprKind::Ident: {
                bool known = local.count(e->name) ||
                             is_builtin_name(e->name) ||
                             func_name_set_.count(e->name) != 0 ||
                             import_alias_set_.count(e->name) != 0 ||
                             using_alias_set_.count(e->name) != 0;
                if (!known) {
                    error(e->tok, "unknown name '" + e->name + "'");
                    note_did_you_mean(e->tok, e->name, local);
                } else if (is_namespace_name(e->name) && !local_is_variable(local, e->name)) {
                    error(e->tok, "'" + e->name + "' is a module name, not a value; write " + e->name + "::name to use something from it");
                } else if (function_depth_ == 0 && module_globals_.count(e->name) != 0 &&
                           top_assigned_.count(e->name) == 0 && func_name_set_.count(e->name) == 0 &&
                           !is_builtin_name(e->name) && using_alias_set_.count(e->name) == 0) {
                    const std::string msg = "'" + e->name + "' is read before it has been given a value";
                    if (loop_depth_ > 0) warn(e->tok, msg + " (it is only set later in this loop, so the first round sees none)");
                    else error(e->tok, msg);
                }
                break;
            }
            case ExprKind::Binary:
                visit_expr(e->kids().left, local);
                visit_expr(e->kids().right, local);
                if ((e->op == TokenType::SLASH || e->op == TokenType::PERCENT) && e->kids().right &&
                    ((e->kids().right->kind == ExprKind::Int && e->kids().right->int_val == 0) ||
                     (e->kids().right->kind == ExprKind::Float && e->kids().right->float_val == 0.0))) {
                    warn(e->tok, std::string(e->op == TokenType::SLASH ? "division" : "remainder") +
                                 " by a literal zero - this always fails when it runs");
                }
                break;
            case ExprKind::Unary:
                visit_expr(e->kids().operand, local);
                break;
            case ExprKind::Assign: {
                visit_expr(e->kids().value, local);
                check_assign_target(e->kids().target, e->tok);
                if (e->kids().target) {
                    if (e->kids().target->kind == ExprKind::Ident) {
                        const_cast<Scope&>(local).insert(e->kids().target->name);
                        note_top_assignment(e->kids().target->name);
                    } else {
                        visit_expr(e->kids().target, local);
                    }
                }
                break;
            }
            case ExprKind::CompoundAssign:
                check_assign_target(e->kids().target, e->tok);
                visit_expr(e->kids().target, local);
                visit_expr(e->kids().value, local);
                break;
            case ExprKind::Call: {
                visit_expr(e->kids().callee, local);
                if (e->kids().callee && e->kids().callee->kind == ExprKind::Ident) {
                    auto ait = func_arity_.find(e->kids().callee->name);
                    if (ait != func_arity_.end() && ait->second != (int)e->kids().args.size()) {
                        std::ostringstream ss;
                        ss << "'" << e->kids().callee->name << "' takes " << ait->second
                           << (ait->second == 1 ? " argument" : " arguments")
                           << ", but this call passes " << e->kids().args.size();
                        error(e->tok, ss.str());
                    }
                }
                for (const auto& a : e->kids().args) visit_expr(a, local);
                break;
            }
            case ExprKind::Member:
                if (!(e->kids().object && e->kids().object->kind == ExprKind::Ident && is_namespace_name(e->kids().object->name)))
                    visit_expr(e->kids().object, local);
                if (e->kids().object && e->kids().object->kind == ExprKind::Ident) {
                    const std::string& mod = e->kids().object->name;
                    auto connected = connected_exports_.find(mod);
                    if (connected != connected_exports_.end()) {
                        if (!connected->second.count(e->name)) {
                            error(e->tok, "'" + e->name + "' is not exposed by connection '" + mod + "'");
                            if (!e->name.empty() && e->name[0] == '_') {
                                result_.diags.note("names that start with '_' are private to their file and are never shared by #connect");
                            } else {
                                std::vector<std::string> shared(connected->second.begin(), connected->second.end());
                                std::sort(shared.begin(), shared.end());
                                auto suggestion = did_you_mean(e->name, shared);
                                if (!suggestion.empty()) result_.diags.note(suggestion);
                                else if (!shared.empty() && shared.size() <= 12) {
                                    std::string list;
                                    for (size_t i = 0; i < shared.size(); ++i) list += (i ? ", " : "") + shared[i];
                                    result_.diags.note("'" + mod + "' shares: " + list);
                                }
                            }
                        }
                        break;
                    }
                    if (import_alias_set_.count(mod) != 0) {
                        std::string modname = alias_to_module_.count(mod) ? alias_to_module_[mod] : mod;
                        if (modname == "basic" && e->name == "matmul") {
                            warn(e->tok, "bc::matmul is deprecated for performance-sensitive AI workloads; it remains supported, but use #inc ai as ai; and ai::matmul with packed f64_array buffers for new code");
                        }
                        if (!module_has_member(modname, e->name)) {
                            auto candidates = module_member_candidates(modname);
                            auto suggestion = did_you_mean(e->name, candidates);
                            const bool known_module = modname == "basic" || is_gpu_module(modname) ||
                                is_tui_module(modname) || is_ai_module(modname) ||
                                is_path_module(modname) || is_fs_module(modname) ||
                                is_process_module(modname) || is_shell_module(modname);
                            if (known_module) error(e->tok, "module '" + modname + "' has no member '" + e->name + "'");
                            else warn(e->tok, "module '" + modname + "' has no member '" + e->name + "'");
                            if (!suggestion.empty()) result_.diags.note(suggestion);
                        }
                    }
                }
                break;
            case ExprKind::Index:
                visit_expr(e->kids().object, local);
                visit_expr(e->kids().index, local);
                break;
            case ExprKind::VectorLiteral:
                for (const auto& el : e->kids().elements) visit_expr(el, local);
                break;
            case ExprKind::DictLiteral: {
                std::unordered_set<std::string> seen_keys;
                for (const auto& kv : e->kids().dict_pairs) {
                    visit_expr(kv.first, local);
                    visit_expr(kv.second, local);
                    if (kv.first && kv.first->kind == ExprKind::String && !seen_keys.insert(kv.first->str_val).second) {
                        warn(kv.first->tok, "duplicate dictionary key \"" + kv.first->str_val + "\" - the later value replaces the earlier one");
                    }
                }
                break;
            }
        }
    }
};

SemanticResult analyze(const Module& mod, const std::string& source,
                        const std::vector<std::string>& builtin_names) {
    Analyzer a(source, builtin_names);
    return a.run(mod);
}

}
