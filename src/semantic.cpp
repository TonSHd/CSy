// Written by Claude
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
            const std::string& alias = s->kind == StmtKind::Connect ? s->connect_alias : s->import_alias;
            auto inserted = namespace_aliases.emplace(alias, s.get());
            if (!inserted.second) {
                error(s->tok, "namespace '" + alias + "' is already declared in this file");
            }
        }
        // Pre-pass: collect top-level function names (with arity) and
        // variable names in declaration order, warning on any collision
        // instead of letting two things silently share one name.
        //
        // Class methods desugar to functions with mangled names like
        // "$Dog$bark" - a collision there always means the *class* name
        // itself also collided (since both instantiations register a
        // constructor function under the plain class name), so mangled
        // names are skipped here to avoid a confusing internal-looking
        // warning on top of the clearer one the user actually caused.
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::Connect) {
                auto inserted = connected_exports_.emplace(s->connect_alias,
                    std::unordered_set<std::string>(s->connect_exports.begin(), s->connect_exports.end()));
                if (!inserted.second) {
                    error(s->tok, "connection namespace '" + s->connect_alias + "' is declared more than once");
                }
            }
            if (s->kind == StmtKind::FuncDecl) {
                bool mangled = !s->func_name.empty() && s->func_name[0] == '$';
                if (!mangled) {
                    auto it = declared_kind_.find(s->func_name);
                    if (it != declared_kind_.end()) {
                        warn(s->tok, "'" + s->func_name + "' is already defined as a " +
                             it->second + " - this redefines it, the earlier one is gone");
                    }
                    declared_kind_[s->func_name] = "function";
                }
                func_names_.push_back(s->func_name);
                func_name_set_.insert(s->func_name);
                func_arity_[s->func_name] = (int)s->params.size();
            }
        }
        // Pre-pass: collect module-level variable names so functions can reference them.
        // This is a single-pass scan - assignments to identifiers at the top level
        // create globals. (Assignments inside if/while/for at top level also count,
        // but we do a simple linear scan for v1.)
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign) {
                auto& target = s->expr->target;
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
                    // [a, b] = ...; - destructuring, register each name.
                    for (const auto& el : target->elements) {
                        if (el && el->kind == ExprKind::Ident) {
                            module_globals_.insert(el->name);
                        }
                    }
                }
            }
        }
        // Module-level scope (shared by all top-level statements).
        Scope module_scope;
        module_scope.names = module_globals_;  // start with known globals
        for (const auto& s : mod.body) {
            visit_stmt(s, module_scope);
        }
        result_.ok = !result_.diags.has_errors();
        return result_;
    }

    // "nvidia" and "cuda" are the same real module under two names - both
    // map to the "nv_"-prefixed builtins in gpu_nvidia.cpp.
    bool is_gpu_module(const std::string& module) {
        return module == "nvidia" || module == "cuda";
    }

    // "tui"'s builtins live under a "tui_" prefix in tui.cpp, same
    // reasoning as is_gpu_module above - tui::color and a hypothetical
    // future bc::color shouldn't collide.
    bool is_tui_module(const std::string& module) {
        return module == "tui";
    }

    bool is_ai_module(const std::string& module) {
        return module == "ai";
    }

    bool is_builtin_name(const std::string& n) {
        return builtin_set_.count(n) > 0;
    }

    // "nvidia"/"tui" builtins are registered in the VM under an "nv_"/
    // "tui_" prefix (see gpu_nvidia.cpp / tui.cpp) so they don't collide
    // with "basic"'s bare names - bc::matmul and nv::matmul are
    // genuinely different functions, not aliases of each other. This
    // maps a module + bare member name to what's actually in the
    // registry.
    bool module_has_member(const std::string& module, const std::string& name) {
        if (module == "basic") return is_builtin_name(name);
        if (is_gpu_module(module)) return is_builtin_name("nv_" + name);
        if (is_tui_module(module)) return is_builtin_name("tui_" + name);
        if (is_ai_module(module)) return is_builtin_name("ai_" + name);
        return false;
    }

    // Did-you-mean candidates, scoped to one module, so a typo in
    // `nv::` only ever suggests other `nv::` members, never `bc::` ones.
    std::vector<std::string> module_member_candidates(const std::string& module) {
        std::vector<std::string> out;
        for (const auto& n : builtin_names_) {
            bool is_nv = n.rfind("nv_", 0) == 0;
            bool is_tui = n.rfind("tui_", 0) == 0;
            bool is_ai = n.rfind("ai_", 0) == 0;
            if (is_gpu_module(module) && is_nv) out.push_back(n.substr(3));
            else if (is_tui_module(module) && is_tui) out.push_back(n.substr(4));
            else if (is_ai_module(module) && is_ai) out.push_back(n.substr(3));
            else if (module == "basic" && !is_nv && !is_tui && !is_ai) out.push_back(n);
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
    std::vector<std::string> active_loop_vars_;   // loop variables of the loops enclosing the current statement

    // Lexically chained scope. A nested function or catch block used to copy the
    // entire enclosing scope (O(globals) per function => quadratic on big
    // projects); now it just points at its parent.
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
        result_.diags.error(t.file(), t.line, t.col, msg);
    }

    // Visits a list of statements, warning once about anything after a statement
    // that always leaves the block (rt / stop / skip): it can never run.
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
        result_.diags.warn(t.file(), t.line, t.col, msg);
    }

    std::vector<std::string> all_known_names(const Scope& local) {
        std::vector<std::string> all;
        for (const Scope* sc = &local; sc; sc = sc->parent)
            for (const auto& n : sc->names) all.push_back(n);
        for (const auto& n : func_names_) all.push_back(n);
        for (const auto& n : import_aliases_) all.push_back(n);
        for (const auto& p : using_aliases_) all.push_back(p.first);
        // Real builtin names, not a hand-maintained copy that drifts out
        // of sync with what the VM actually registers.
        for (const auto& n : builtin_names_) all.push_back(n);
        return all;
    }

    void note_did_you_mean(const Token& t, const std::string& target, const Scope& local) {
        // A name that a #connect exports is reachable only through its namespace: say so.
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

    void visit_stmt(const StmtPtr& s, Scope& local) {
        res::poll("semantic analysis");
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Import: {
                import_aliases_.push_back(s->import_alias);
                import_alias_set_.insert(s->import_alias);
                alias_to_module_[s->import_alias] = s->import_module;
                result_.import_aliases.push_back(s->import_alias);
                if (is_gpu_module(s->import_module)) {
                    warn(s->tok,
                         "the 'nvidia' and 'cuda' imports are deprecated and remain supported for compatibility; for MLP training, use '#inc ai as ai;' and ai::mlp_train_gpu. Existing nv:: hardware and matmul functions still work");
                }
                if (s->import_module != "basic" && !is_gpu_module(s->import_module) &&
                    !is_tui_module(s->import_module) && !is_ai_module(s->import_module)) {
                    warn(s->tok, "unknown module '" + s->import_module +
                         "', only 'basic', 'nvidia', 'cuda', 'tui', and 'ai' are supported. The alias '" +
                         s->import_alias + "' will be created but it has no members.");
                }
                break;
            }
            case StmtKind::Connect:
                import_aliases_.push_back(s->connect_alias);
                import_alias_set_.insert(s->connect_alias);
                local.insert(s->connect_alias);
                break;
            case StmtKind::Explain:
            case StmtKind::DirectiveAlias:
                break;
            case StmtKind::Using: {
                // Validate that using_expr resolves to something known.
                // For v1, only support `using <alias>.out as <name>` and similar simple forms.
                if (s->using_expr && s->using_expr->kind == ExprKind::Member) {
                    const auto& obj = s->using_expr->object;
                    if (obj && obj->kind == ExprKind::Ident) {
                        // OK - e.g. using bc.out as o;
                        auto module = alias_to_module_.find(obj->name);
                        if (module != alias_to_module_.end() &&
                            module->second == "basic" && s->using_expr->name == "matmul") {
                            warn(s->tok, "using basic.matmul is deprecated for performance-sensitive AI workloads; it remains supported, but use ai::matmul with packed f64_array buffers for new code");
                        }
                        using_aliases_.push_back({s->using_alias, s->using_expr->name});
                using_alias_set_.insert(s->using_alias);
                        result_.using_aliases.push_back({s->using_alias, s->using_expr->name});
                        local.insert(s->using_alias);
                    } else {
                        warn(s->tok, "unsupported 'using' target - only `using <module>.<member> as <name>` is supported in v1");
                    }
                } else {
                    warn(s->tok, "unsupported 'using' target - only `using <module>.<member> as <name>` is supported in v1");
                }
                break;
            }
            case StmtKind::VarDecl: {
                // x = expr; - the LHS is an IdentExpr that creates a new var,
                // or a VectorLiteral of Idents for destructuring: [a,b] = ...
                // Visit RHS first (so RHS can't see the new name(s) yet).
                if (s->expr && s->expr->kind == ExprKind::Assign) {
                    visit_expr(s->expr->value, local);
                    auto& target = s->expr->target;
                    if (target && target->kind == ExprKind::Ident) {
                        if (s->expr->value && s->expr->value->kind == ExprKind::Ident &&
                            s->expr->value->name == target->name) {
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
                        for (const auto& el : target->elements) {
                            if (el && el->kind == ExprKind::Ident) local.insert(el->name);
                        }
                    }
                } else {
                    visit_expr(s->expr, local);
                }
                break;
            }
            case StmtKind::ExprStmt:
                visit_expr(s->expr, local);
                break;
            case StmtKind::FuncDecl: {
                // Nested function declarations become real closures: they
                // can see everything the enclosing scope can see (locals,
                // params, outer closures' captures), not just module
                // globals. Register the function's own name too, so it's
                // callable - including recursively from within itself -
                // both here and by sibling statements that follow.
                // (At the module level `local` already *is* the running
                // module scope, so this is also correct - and unchanged in
                // behavior - for ordinary top-level functions.)
                local.insert(s->func_name);
                Scope fn_scope;
                fn_scope.parent = &local;
                for (const auto& p : s->params) fn_scope.insert(p);
                // `stop`/`skip` inside a function never refers to a loop that
                // encloses the function's *definition*.
                const int saved_loop_depth = loop_depth_;
                loop_depth_ = 0;
                std::vector<std::string> saved_loop_vars;
                saved_loop_vars.swap(active_loop_vars_);
                visit_block(s->body, fn_scope);
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
                // Branch bodies share the parent scope (vars declared in a branch
                // leak to subsequent branches - v1 simplicity).
                visit_block(s->then_body, local);
                for (const auto& el : s->elifs) {
                    visit_expr(el.first, local);
                    visit_block(el.second, local);
                }
                if (!s->else_body.empty()) {
                    visit_block(s->else_body, local);
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
                visit_expr(s->iterable, local);
                local.insert(s->loop_var);
                loop_depth_++;
                active_loop_vars_.push_back(s->loop_var);
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
                // catch_var is only bound within the catch block.
                Scope catch_scope;
                catch_scope.parent = &local;
                catch_scope.insert(s->catch_var);
                visit_block(s->then_body, catch_scope);
                break;
            }
            case StmtKind::Block:
                visit_block(s->body, local);
                break;
        }
    }

    void visit_expr(const ExprPtr& e, const Scope& local) {
        if (!e) return;
        // Note: `local` passed by const ref - for VarDecl, the parent visit_stmt
        // handles the actual scope mutation.
        switch (e->kind) {
            case ExprKind::Int:
            case ExprKind::Float:
            case ExprKind::String:
            case ExprKind::Bool:
            case ExprKind::None:
                break;
            case ExprKind::Ident: {
                // Check if known.
                bool known = local.count(e->name) ||
                             is_builtin_name(e->name) ||
                             func_name_set_.count(e->name) != 0 ||
                             import_alias_set_.count(e->name) != 0 ||
                             using_alias_set_.count(e->name) != 0;
                if (!known) {
                    error(e->tok, "unknown name '" + e->name + "'");
                    note_did_you_mean(e->tok, e->name, local);
                }
                break;
            }
            case ExprKind::Binary:
                visit_expr(e->left, local);
                visit_expr(e->right, local);
                if ((e->op == TokenType::SLASH || e->op == TokenType::PERCENT) && e->right &&
                    ((e->right->kind == ExprKind::Int && e->right->int_val == 0) ||
                     (e->right->kind == ExprKind::Float && e->right->float_val == 0.0))) {
                    warn(e->tok, std::string(e->op == TokenType::SLASH ? "division" : "remainder") +
                                 " by a literal zero - this always fails when it runs");
                }
                break;
            case ExprKind::Unary:
                visit_expr(e->operand, local);
                break;
            case ExprKind::Assign: {
                visit_expr(e->value, local);
                // Target can be Ident, Member, or Index.
                if (e->target) {
                    if (e->target->kind == ExprKind::Ident) {
                        // Implicit declaration - add to scope.
                        // (We don't mutate the caller's scope here, but we treat
                        //  it as known going forward in this expression.)
                        const_cast<Scope&>(local).insert(e->target->name);
                    } else {
                        visit_expr(e->target, local);
                    }
                }
                break;
            }
            case ExprKind::CompoundAssign:
                visit_expr(e->target, local);
                visit_expr(e->value, local);
                break;
            case ExprKind::Call: {
                visit_expr(e->callee, local);
                // Arity check - scoped deliberately to plain `name(args)`
                // calls. Instance method calls (`obj::method(args)`) auto
                // -inject the instance as an implicit first argument the
                // user never writes, so naively comparing arg counts there
                // would misfire; that shape is skipped here on purpose.
                if (e->callee && e->callee->kind == ExprKind::Ident) {
                    auto ait = func_arity_.find(e->callee->name);
                    if (ait != func_arity_.end() && ait->second != (int)e->args.size()) {
                        std::ostringstream ss;
                        ss << "'" << e->callee->name << "' takes " << ait->second
                           << (ait->second == 1 ? " argument" : " arguments")
                           << ", but this call passes " << e->args.size();
                        warn(e->tok, ss.str());
                    }
                }
                for (const auto& a : e->args) visit_expr(a, local);
                break;
            }
            case ExprKind::Member:
                visit_expr(e->object, local);
                // alias::name - validate against the real builtin list if the
                // object is a genuine `#inc` alias. Anything else (obj::field)
                // is dict/instance access on a runtime value - nothing to
                // statically check, so no warning.
                if (e->object && e->object->kind == ExprKind::Ident) {
                    const std::string& mod = e->object->name;
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
                            warn(e->tok, "module '" + modname + "' has no member '" + e->name + "'");
                            if (!suggestion.empty()) result_.diags.note(suggestion);
                        }
                    }
                }
                break;
            case ExprKind::Index:
                visit_expr(e->object, local);
                visit_expr(e->index, local);
                break;
            case ExprKind::VectorLiteral:
                for (const auto& el : e->elements) visit_expr(el, local);
                break;
            case ExprKind::DictLiteral: {
                std::unordered_set<std::string> seen_keys;
                for (const auto& kv : e->dict_pairs) {
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

} // namespace csy
