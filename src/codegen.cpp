#include "codegen.hpp"
#include "object_file.hpp"
#include "resource.hpp"
#include "builtins_ext.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>


namespace csy {

class Codegen {
public:
    Codegen(const std::vector<std::pair<std::string,std::string>>& using_aliases,
            const std::string& source, const std::vector<std::string>& builtin_names)
        : source_(source), using_aliases_(using_aliases), builtin_names_(builtin_names) {}

    CodegenResult run(const Module& mod) {
        ir_.file = mod.file;
        for (const auto& b : builtin_names_) {
            ir_.add_builtin(b);
        }
        IRFunction main_fn;
        main_fn.name = "<main>";
        ir_.functions.push_back(std::move(main_fn));
        ir_.main_func_index = 0;

        ir_.functions.reserve(2 + count_all_funcdecls(mod.body));

        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::Import) {
                import_aliases_[s->aux().import_alias] = s->aux().import_module;
            } else if (s->kind == StmtKind::Connect) {
                connected_aliases_[s->aux().connect_alias] = std::unordered_set<std::string>(
                    s->aux().connect_exports.begin(), s->aux().connect_exports.end());
            }
        }

        std::unordered_map<std::string, int> func_indices;
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::FuncDecl) {
                toplevel_func_stmts_.insert(s.get());
                IRFunction fn;
                fn.name = s->aux().func_name;
                fn.param_names = s->aux().params;
                fn.num_locals = (int)s->aux().params.size();
                for (int i = 0; i < (int)s->aux().params.size(); i++) {
                    fn.local_slots[s->aux().params[i]] = i;
                }
                int idx = (int)ir_.functions.size();
                ir_.functions.push_back(std::move(fn));
                func_indices[s->aux().func_name] = idx;
                int gidx = ir_.add_global(s->aux().func_name);
                func_global_indices_[s->aux().func_name] = gidx;
            }
        }
        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign) {
                auto& target = s->expr->kids().target;
                if (target && target->kind == ExprKind::Ident) {
                    ir_.add_global(target->name);
                }
            }
        }

        for (const auto& s : mod.body) {
            if (s->kind == StmtKind::FuncDecl) {
                int idx = func_indices[s->aux().func_name];
                compile_function_body(ir_.functions[idx], s->body, func_indices);
            }
        }
        compile_function_body(ir_.functions[0], mod.body, func_indices);
        emit(ir_.functions[0], Op::HALT, 0, 0, 0);

        if (!result_.diags.has_errors()) {
            std::string why;
            if (!verify_module(ir_, why)) {
                result_.diags.error(ir_.file, 0, 0, why);
            }
        }
        result_.ok = !result_.diags.has_errors();
        result_.module = std::move(ir_);
        return result_;
    }

private:
    int compound_tmp_counter_ = 0;

    int count_all_funcdecls(const std::vector<StmtPtr>& body) {
        int n = 0;
        for (const auto& s : body) n += count_all_funcdecls_stmt(s);
        return n;
    }
    int count_all_funcdecls_stmt(const StmtPtr& s) {
        if (!s) return 0;
        if (s->kind == StmtKind::FuncDecl) return 1 + count_all_funcdecls(s->body);
        switch (s->kind) {
            case StmtKind::If: {
                int n = 0;
                for (const auto& bs : s->aux().then_body) n += count_all_funcdecls_stmt(bs);
                for (const auto& el : s->aux().elifs)
                    for (const auto& bs : el.second) n += count_all_funcdecls_stmt(bs);
                for (const auto& bs : s->aux().else_body) n += count_all_funcdecls_stmt(bs);
                return n;
            }
            case StmtKind::While:
            case StmtKind::For:
            case StmtKind::Block:
                return count_all_funcdecls(s->body);
            case StmtKind::Try:
                return count_all_funcdecls(s->body) + count_all_funcdecls(s->aux().then_body);
            default:
                return 0;
        }
    }

    CodegenResult result_;
    IRModule ir_;
    const std::string& source_;
    const std::vector<std::pair<std::string,std::string>>& using_aliases_;
    const std::vector<std::string>& builtin_names_;
    std::unordered_map<std::string, int> func_global_indices_;
    std::unordered_map<std::string, std::string> import_aliases_;
    std::unordered_map<std::string, std::unordered_set<std::string>> connected_aliases_;
    std::unordered_set<const Stmt*> toplevel_func_stmts_;
    std::vector<std::string> self_name_stack_;

    struct LoopCtx {
        std::vector<int> break_patches;
        std::vector<int> continue_patches;
        int try_depth_at_entry = 0;
    };
    std::vector<LoopCtx> loop_stack_;
    int try_depth_ = 0;
    LoopCtx make_loop_ctx() { LoopCtx c; c.try_depth_at_entry = try_depth_; return c; }
    void emit_try_exits_for_jump(IRFunction& fn, int line, int col) {
        for (int k = try_depth_; k > loop_stack_.back().try_depth_at_entry; --k) {
            emit(fn, Op::TRY_EXIT, 0, line, col);
        }
    }
    bool is_lazy_range(const IRFunction& fn, const ExprPtr& it,
                       const std::unordered_map<std::string, int>& func_indices) const {
        if (!it || it->kind != ExprKind::Call || !it->kids().callee ||
            it->kids().callee->kind != ExprKind::Ident || it->kids().callee->name != "range" ||
            it->kids().args.size() != 1) return false;
        const std::string& name = it->kids().callee->name;
        if (fn.local_slots.count(name) > 0 || fn.upvalue_slots.count(name) > 0) return false;
        if (!self_name_stack_.empty() && self_name_stack_.back() == name) return false;
        if (func_indices.count(name) > 0) return false;
        return ir_.builtin_index.count("range") > 0 && ir_.builtin_index.count("__range_bound") > 0;
    }
    bool emit_inline_builtin(IRFunction& fn, const std::string& key, size_t nargs, const Token& tok) {
        if (key == "len" && nargs == 1) { emit(fn, Op::LEN, 0, tok.line, tok.col); return true; }
        if (key == "push" && nargs == 2) { emit(fn, Op::VEC_PUSH, 0, tok.line, tok.col); return true; }
        return false;
    }
    int pack_native_call(int builtin_index, size_t nargs, const Token& tok) {
        if (nargs >= 1000) {
            result_.diags.error(tok.file(), tok.line, tok.col,
                "a built-in function call may have at most 999 arguments (this one has " +
                std::to_string(nargs) + ")");
            nargs = 0;
        }
        return builtin_index * 1000 + static_cast<int>(nargs);
    }
    bool at_stmt_top_ = false;

    bool is_module_alias(const ExprPtr& e) {
        return e && e->kind == ExprKind::Ident && import_aliases_.count(e->name) > 0;
    }

    std::string builtin_key_for(const ExprPtr& alias_ident, const std::string& member) {
        auto it = import_aliases_.find(alias_ident->name);
        if (it != import_aliases_.end()) {
            if (it->second == "nvidia" || it->second == "cuda") return "nv_" + member;
            if (it->second == "tui") return "tui_" + member;
            if (it->second == "ai") return "ai_" + member;
            if (it->second == "path") return "path_" + member;
            if (it->second == "fs") return "fs_" + member;
            if (it->second == "process") return "process_" + member;
            if (it->second == "shell") return "shell_" + member;
        }
        return member;
    }

    bool needs_process_site(const std::string& name) const {
        return name == "process_run" || name == "shell_run" || name == "shell_interactive";
    }

    void emit_process_site(IRFunction& fn, const ExprPtr& e) {
        emit(fn, Op::PUSH_STR, ir_.add_str(e->aux().process_project_root), e->tok.line, e->tok.col);
        emit(fn, Op::PUSH_STR, ir_.add_str(e->aux().process_site_key), e->tok.line, e->tok.col);
        emit(fn, Op::PUSH_STR, ir_.add_str(e->aux().process_site_location), e->tok.line, e->tok.col);
    }

    ExprPtr member_as_index(const ExprPtr& m) {
        auto key = make_expr(ExprKind::String, m->tok);
        key->str_val = m->name;
        auto idx = make_expr(ExprKind::Index, m->tok);
        idx->kids_mut().object = m->kids_mut().object;
        idx->kids_mut().index = key;
        return idx;
    }

    void store_to_ident(IRFunction& fn, const std::string& name, int line, int col) {
        auto lit = fn.local_slots.find(name);
        bool is_local = (lit != fn.local_slots.end());
        bool is_global = (ir_.global_index.find(name) != ir_.global_index.end());
        auto uit = fn.upvalue_slots.find(name);
        bool is_upvalue = (uit != fn.upvalue_slots.end());
        if (fn.name == "<main>") {
            int g = ir_.add_global(name);
            emit(fn, Op::STORE_GLOBAL, g, line, col);
        } else if (is_local) {
            if (fn.boxed_locals.count(name)) {
                emit(fn, Op::STORE_LOCAL_BOXED, lit->second, line, col);
            } else {
                emit(fn, Op::STORE_LOCAL, lit->second, line, col);
            }
        } else if (is_upvalue) {
            emit(fn, Op::STORE_UPVALUE, uit->second, line, col);
        } else if (is_global) {
            int g = ir_.global_index[name];
            emit(fn, Op::STORE_GLOBAL, g, line, col);
        } else {
            int slot = alloc_local(fn, name);
            emit(fn, Op::STORE_LOCAL, slot, line, col);
        }
    }

    bool try_load_self_or_upvalue(IRFunction& fn, const std::string& name, int line, int col) {
        if (!self_name_stack_.empty() && self_name_stack_.back() == name) {
            emit(fn, Op::PUSH_SELF, 0, line, col);
            return true;
        }
        auto uit = fn.upvalue_slots.find(name);
        if (uit != fn.upvalue_slots.end()) {
            emit(fn, Op::LOAD_UPVALUE, uit->second, line, col);
            return true;
        }
        return false;
    }

    void emit(IRFunction& fn, Op op, int arg, int line, int col) {
        Instr ins;
        ins.op = op;
        ins.arg = arg;
        ins.line = line;
        ins.col = col;
        fn.code.push_back(ins);
    }

    void compile_function_body(IRFunction& fn, const std::vector<StmtPtr>& body,
                                const std::unordered_map<std::string, int>& func_indices) {
        if (fn.name != "<main>") {
            fn.boxed_locals = compute_boxed_locals(fn.param_names, body);
            for (const auto& name : fn.boxed_locals) {
                auto it = fn.local_slots.find(name);
                if (it != fn.local_slots.end()) {
                    int slot = it->second;
                    emit(fn, Op::LOAD_LOCAL, slot, 0, 0);
                    emit(fn, Op::MAKE_VEC, 1, 0, 0);
                    emit(fn, Op::STORE_LOCAL, slot, 0, 0);
                } else {
                    int slot = alloc_local(fn, name);
                    emit(fn, Op::PUSH_NONE, 0, 0, 0);
                    emit(fn, Op::MAKE_VEC, 1, 0, 0);
                    emit(fn, Op::STORE_LOCAL, slot, 0, 0);
                }
            }
        }
        for (const auto& s : body) {
            compile_stmt(fn, s, func_indices);
        }
        emit(fn, Op::PUSH_NONE, 0, 0, 0);
        emit(fn, Op::RET, 0, 0, 0);
    }

    int alloc_local(IRFunction& fn, const std::string& name) {
        auto it = fn.local_slots.find(name);
        if (it != fn.local_slots.end()) return it->second;
        int slot = fn.num_locals++;
        fn.local_slots[name] = slot;
        return slot;
    }

    void collect_bound_names(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& bound) {
        for (const auto& s : body) collect_bound_names_stmt(s, bound);
    }
    void collect_bound_names_stmt(const StmtPtr& s, std::unordered_set<std::string>& bound) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::FuncDecl:
                bound.insert(s->aux().func_name);
                for (const auto& p : s->aux().params) bound.insert(p);
                collect_bound_names(s->body, bound);
                break;
            case StmtKind::If:
                for (const auto& bs : s->aux().then_body) collect_bound_names_stmt(bs, bound);
                for (const auto& el : s->aux().elifs)
                    for (const auto& bs : el.second) collect_bound_names_stmt(bs, bound);
                for (const auto& bs : s->aux().else_body) collect_bound_names_stmt(bs, bound);
                break;
            case StmtKind::While:
                collect_bound_names(s->body, bound);
                break;
            case StmtKind::For:
                bound.insert(s->aux().loop_var);
                collect_bound_names(s->body, bound);
                break;
            case StmtKind::Try:
                bound.insert(s->aux().catch_var);
                collect_bound_names(s->body, bound);
                collect_bound_names(s->aux().then_body, bound);
                break;
            case StmtKind::Block:
                collect_bound_names(s->body, bound);
                break;
            default:
                break;
        }
    }

    void note_ref(const std::string& name, std::vector<std::string>& order, std::unordered_set<std::string>& seen) {
        if (seen.insert(name).second) order.push_back(name);
    }
    void collect_referenced_idents(const std::vector<StmtPtr>& body,
                                    std::vector<std::string>& order, std::unordered_set<std::string>& seen) {
        for (const auto& s : body) collect_referenced_idents_stmt(s, order, seen);
    }
    void collect_referenced_idents_expr(const ExprPtr& e,
                                         std::vector<std::string>& order, std::unordered_set<std::string>& seen) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Int: case ExprKind::Float: case ExprKind::String:
            case ExprKind::Bool: case ExprKind::None:
                break;
            case ExprKind::Ident:
                note_ref(e->name, order, seen);
                break;
            case ExprKind::Binary:
                collect_referenced_idents_expr(e->kids().left, order, seen);
                collect_referenced_idents_expr(e->kids().right, order, seen);
                break;
            case ExprKind::Unary:
                collect_referenced_idents_expr(e->kids().operand, order, seen);
                break;
            case ExprKind::Assign:
                collect_referenced_idents_expr(e->kids().value, order, seen);
                if (e->kids().target && e->kids().target->kind == ExprKind::Ident) {
                    note_ref(e->kids().target->name, order, seen);
                } else if (e->kids().target) {
                    collect_referenced_idents_expr(e->kids().target, order, seen);
                }
                break;
            case ExprKind::CompoundAssign:
                collect_referenced_idents_expr(e->kids().target, order, seen);
                collect_referenced_idents_expr(e->kids().value, order, seen);
                break;
            case ExprKind::Call:
                collect_referenced_idents_expr(e->kids().callee, order, seen);
                for (const auto& a : e->kids().args) collect_referenced_idents_expr(a, order, seen);
                break;
            case ExprKind::Member:
                collect_referenced_idents_expr(e->kids().object, order, seen);
                break;
            case ExprKind::Index:
                collect_referenced_idents_expr(e->kids().object, order, seen);
                collect_referenced_idents_expr(e->kids().index, order, seen);
                break;
            case ExprKind::VectorLiteral:
                for (const auto& el : e->kids().elements) collect_referenced_idents_expr(el, order, seen);
                break;
            case ExprKind::DictLiteral:
                for (const auto& kv : e->kids().dict_pairs) {
                    collect_referenced_idents_expr(kv.first, order, seen);
                    collect_referenced_idents_expr(kv.second, order, seen);
                }
                break;
        }
    }
    void collect_referenced_idents_stmt(const StmtPtr& s,
                                         std::vector<std::string>& order, std::unordered_set<std::string>& seen) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::VarDecl:
                if (s->expr && s->expr->kind == ExprKind::Assign) {
                    collect_referenced_idents_expr(s->expr->kids().value, order, seen);
                    auto& t = s->expr->kids().target;
                    if (t && t->kind == ExprKind::Ident) {
                        note_ref(t->name, order, seen);
                    } else if (t && t->kind == ExprKind::VectorLiteral) {
                        for (const auto& el : t->kids().elements)
                            if (el && el->kind == ExprKind::Ident) note_ref(el->name, order, seen);
                    } else if (t) {
                        collect_referenced_idents_expr(t, order, seen);
                    }
                } else {
                    collect_referenced_idents_expr(s->expr, order, seen);
                }
                break;
            case StmtKind::ExprStmt:
                collect_referenced_idents_expr(s->expr, order, seen);
                break;
            case StmtKind::FuncDecl:
                collect_referenced_idents(s->body, order, seen);
                break;
            case StmtKind::If:
                collect_referenced_idents_expr(s->cond, order, seen);
                for (const auto& bs : s->aux().then_body) collect_referenced_idents_stmt(bs, order, seen);
                for (const auto& el : s->aux().elifs) {
                    collect_referenced_idents_expr(el.first, order, seen);
                    for (const auto& bs : el.second) collect_referenced_idents_stmt(bs, order, seen);
                }
                for (const auto& bs : s->aux().else_body) collect_referenced_idents_stmt(bs, order, seen);
                break;
            case StmtKind::While:
                collect_referenced_idents_expr(s->cond, order, seen);
                collect_referenced_idents(s->body, order, seen);
                break;
            case StmtKind::For:
                collect_referenced_idents_expr(s->aux().iterable, order, seen);
                collect_referenced_idents(s->body, order, seen);
                break;
            case StmtKind::Return:
                collect_referenced_idents_expr(s->ret_value, order, seen);
                break;
            case StmtKind::Try:
                collect_referenced_idents(s->body, order, seen);
                collect_referenced_idents(s->aux().then_body, order, seen);
                break;
            case StmtKind::Block:
                collect_referenced_idents(s->body, order, seen);
                break;
            default:
                break;
        }
    }

    void collect_own_bound_names(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& bound) {
        for (const auto& s : body) collect_own_bound_names_stmt(s, bound);
    }
    void collect_own_bound_names_stmt(const StmtPtr& s, std::unordered_set<std::string>& bound) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::VarDecl:
                if (s->expr && s->expr->kind == ExprKind::Assign && s->expr->kids().target) {
                    auto& t = s->expr->kids().target;
                    if (t->kind == ExprKind::Ident) bound.insert(t->name);
                    else if (t->kind == ExprKind::VectorLiteral) {
                        for (const auto& el : t->kids().elements)
                            if (el && el->kind == ExprKind::Ident) bound.insert(el->name);
                    }
                }
                break;
            case StmtKind::ExprStmt:
                if (s->expr && s->expr->kind == ExprKind::CompoundAssign && s->expr->kids().target &&
                    s->expr->kids().target->kind == ExprKind::Ident) {
                    bound.insert(s->expr->kids().target->name);
                }
                break;
            case StmtKind::FuncDecl:
                bound.insert(s->aux().func_name);
                break;
            case StmtKind::If:
                for (const auto& bs : s->aux().then_body) collect_own_bound_names_stmt(bs, bound);
                for (const auto& el : s->aux().elifs)
                    for (const auto& bs : el.second) collect_own_bound_names_stmt(bs, bound);
                for (const auto& bs : s->aux().else_body) collect_own_bound_names_stmt(bs, bound);
                break;
            case StmtKind::While:
                collect_own_bound_names(s->body, bound);
                break;
            case StmtKind::For:
                bound.insert(s->aux().loop_var);
                collect_own_bound_names(s->body, bound);
                break;
            case StmtKind::Try:
                bound.insert(s->aux().catch_var);
                collect_own_bound_names(s->body, bound);
                collect_own_bound_names(s->aux().then_body, bound);
                break;
            case StmtKind::Block:
                collect_own_bound_names(s->body, bound);
                break;
            default:
                break;
        }
    }

    void find_nested_funcdecls(const std::vector<StmtPtr>& body, std::vector<const Stmt*>& out) {
        for (const auto& s : body) find_nested_funcdecls_stmt(s, out);
    }
    void find_nested_funcdecls_stmt(const StmtPtr& s, std::vector<const Stmt*>& out) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::FuncDecl:
                out.push_back(s.get());
                find_nested_funcdecls(s->body, out);
                break;
            case StmtKind::If:
                for (const auto& bs : s->aux().then_body) find_nested_funcdecls_stmt(bs, out);
                for (const auto& el : s->aux().elifs)
                    for (const auto& bs : el.second) find_nested_funcdecls_stmt(bs, out);
                for (const auto& bs : s->aux().else_body) find_nested_funcdecls_stmt(bs, out);
                break;
            case StmtKind::While:
            case StmtKind::For:
            case StmtKind::Block:
                find_nested_funcdecls(s->body, out);
                break;
            case StmtKind::Try:
                find_nested_funcdecls(s->body, out);
                find_nested_funcdecls(s->aux().then_body, out);
                break;
            default:
                break;
        }
    }

    std::unordered_set<std::string> compute_boxed_locals(const std::vector<std::string>& params,
                                                            const std::vector<StmtPtr>& body) {
        std::unordered_set<std::string> own_bound(params.begin(), params.end());
        collect_own_bound_names(body, own_bound);

        std::vector<const Stmt*> nested;
        find_nested_funcdecls(body, nested);

        std::unordered_set<std::string> boxed;
        for (const Stmt* nd : nested) {
            std::vector<std::string> ref_order;
            std::unordered_set<std::string> ref_seen;
            collect_referenced_idents(nd->body, ref_order, ref_seen);
            for (const auto& name : ref_order) {
                if (own_bound.count(name)) boxed.insert(name);
            }
        }
        return boxed;
    }

    void compile_stmt(IRFunction& fn, const StmtPtr& s,
                       const std::unordered_map<std::string, int>& func_indices) {
        res::poll("code generation");
        if (!s) return;
        switch (s->kind) {
            case StmtKind::Import:
            case StmtKind::Using:
            case StmtKind::Connect:
            case StmtKind::Explain:
            case StmtKind::DirectiveAlias:
                break;
            case StmtKind::VarDecl: {
                if (s->expr->kind == ExprKind::Assign) {
                    auto target = s->expr->kids().target;
                    if (target->kind == ExprKind::Member && !is_module_alias(target->kids().object)) {
                        if (!(target->kids().object && target->kids().object->kind == ExprKind::Ident &&
                              connected_aliases_.count(target->kids().object->name) &&
                              connected_aliases_[target->kids().object->name].count(target->name))) {
                            target = member_as_index(target);
                        }
                    }
                    if (target->kind == ExprKind::Ident) {
                        compile_expr(fn, s->expr->kids().value, func_indices);
                        store_to_ident(fn, target->name, target->tok.line, target->tok.col);
                    } else if (target->kind == ExprKind::Index) {
                        compile_expr(fn, target->kids().object, func_indices);
                        compile_expr(fn, target->kids().index, func_indices);
                        compile_expr(fn, s->expr->kids().value, func_indices);
                        emit(fn, Op::SET_INDEX, 0, target->tok.line, target->tok.col);
                    } else if (target->kind == ExprKind::Member &&
                               target->kids().object && target->kids().object->kind == ExprKind::Ident &&
                               connected_aliases_.count(target->kids().object->name) &&
                               connected_aliases_[target->kids().object->name].count(target->name)) {
                        compile_expr(fn, s->expr->kids().value, func_indices);
                        emit(fn, Op::STORE_GLOBAL, ir_.add_global(target->name), target->tok.line, target->tok.col);
                    } else if (target->kind == ExprKind::VectorLiteral) {
                        compile_expr(fn, s->expr->kids().value, func_indices);
                        int tmp = alloc_local(fn, "$destructure_tmp" + std::to_string(++compound_tmp_counter_));
                        emit(fn, Op::STORE_LOCAL, tmp, target->tok.line, target->tok.col);
                        for (size_t i = 0; i < target->kids().elements.size(); i++) {
                            const auto& elem = target->kids().elements[i];
                            if (!elem || elem->kind != ExprKind::Ident) {
                                result_.diags.error(ir_.file, target->tok.line, target->tok.col,
                                    "destructuring targets must be plain names, like [a, b] = ...");
                                continue;
                            }
                            emit(fn, Op::LOAD_LOCAL, tmp, elem->tok.line, elem->tok.col);
                            emit(fn, Op::PUSH_INT, ir_.add_int((long long)i), elem->tok.line, elem->tok.col);
                            emit(fn, Op::GET_INDEX, 0, elem->tok.line, elem->tok.col);
                            store_to_ident(fn, elem->name, elem->tok.line, elem->tok.col);
                        }
                    }
                }
                break;
            }
            case StmtKind::ExprStmt: {
                at_stmt_top_ = true;
                compile_expr(fn, s->expr, func_indices);
                if (s->expr->kind == ExprKind::Assign ||
                    s->expr->kind == ExprKind::CompoundAssign) {
                } else {
                    emit(fn, Op::POP, 0, s->tok.line, s->tok.col);
                }
                break;
            }
            case StmtKind::FuncDecl: {
                if (toplevel_func_stmts_.count(s.get())) {
                    break;
                }
                std::unordered_set<std::string> bound;
                for (const auto& p : s->aux().params) bound.insert(p);
                collect_bound_names(s->body, bound);
                std::vector<std::string> ref_order;
                std::unordered_set<std::string> ref_seen;
                collect_referenced_idents(s->body, ref_order, ref_seen);

                IRFunction nested;
                nested.name = s->aux().func_name;
                nested.param_names = s->aux().params;
                nested.num_locals = (int)s->aux().params.size();
                for (int i = 0; i < (int)s->aux().params.size(); i++) {
                    nested.local_slots[s->aux().params[i]] = i;
                }
                for (const auto& name : ref_order) {
                    if (bound.count(name)) continue;
                    bool capturable = fn.local_slots.count(name) > 0 || fn.upvalue_slots.count(name) > 0;
                    if (!capturable) continue;
                    nested.upvalue_slots[name] = (int)nested.upvalue_names.size();
                    nested.upvalue_names.push_back(name);
                }

                int nested_idx = (int)ir_.functions.size();
                ir_.functions.push_back(std::move(nested));

                for (const auto& name : ir_.functions[nested_idx].upvalue_names) {
                    auto lit = fn.local_slots.find(name);
                    if (lit != fn.local_slots.end()) {
                        emit(fn, Op::LOAD_LOCAL, lit->second, s->tok.line, s->tok.col);
                    } else {
                        emit(fn, Op::LOAD_UPVALUE_BOX, fn.upvalue_slots[name], s->tok.line, s->tok.col);
                    }
                }
                emit(fn, Op::MAKE_CLOSURE, nested_idx, s->tok.line, s->tok.col);
                store_to_ident(fn, s->aux().func_name, s->tok.line, s->tok.col);

                self_name_stack_.push_back(s->aux().func_name);
                {
                    std::vector<LoopCtx> saved_loops;
                    saved_loops.swap(loop_stack_);
                    const int saved_try_depth = try_depth_;
                    try_depth_ = 0;
                    compile_function_body(ir_.functions[nested_idx], s->body, func_indices);
                    loop_stack_.swap(saved_loops);
                    try_depth_ = saved_try_depth;
                }
                self_name_stack_.pop_back();
                break;
            }
            case StmtKind::If: {
                compile_expr(fn, s->cond, func_indices);
                int jmpf_pos = (int)fn.code.size();
                emit(fn, Op::JMPF, 0, s->tok.line, s->tok.col);
                for (const auto& bs : s->aux().then_body) compile_stmt(fn, bs, func_indices);
                std::vector<int> jmps_to_end;
                if (!s->aux().elifs.empty() || !s->aux().else_body.empty()) {
                    jmps_to_end.push_back((int)fn.code.size());
                    emit(fn, Op::JMP, 0, s->tok.line, s->tok.col);
                }
                fn.code[jmpf_pos].arg = (int)fn.code.size();
                for (const auto& el : s->aux().elifs) {
                    compile_expr(fn, el.first, func_indices);
                    int jf = (int)fn.code.size();
                    emit(fn, Op::JMPF, 0, el.first->tok.line, el.first->tok.col);
                    for (const auto& bs : el.second) compile_stmt(fn, bs, func_indices);
                    int jend = (int)fn.code.size();
                    emit(fn, Op::JMP, 0, el.first->tok.line, el.first->tok.col);
                    fn.code[jf].arg = (int)fn.code.size();
                    jmps_to_end.push_back(jend);
                }
                if (!s->aux().else_body.empty()) {
                    for (const auto& bs : s->aux().else_body) compile_stmt(fn, bs, func_indices);
                }
                int end_pos = (int)fn.code.size();
                for (int j : jmps_to_end) fn.code[j].arg = end_pos;
                break;
            }
            case StmtKind::Try: {
                int try_enter_pos = (int)fn.code.size();
                emit(fn, Op::TRY_ENTER, 0, s->tok.line, s->tok.col);
                ++try_depth_;
                for (const auto& bs : s->body) compile_stmt(fn, bs, func_indices);
                --try_depth_;
                emit(fn, Op::TRY_EXIT, 0, s->tok.line, s->tok.col);
                int jmp_end_pos = (int)fn.code.size();
                emit(fn, Op::JMP, 0, s->tok.line, s->tok.col);
                fn.code[try_enter_pos].arg = (int)fn.code.size();
                store_to_ident(fn, s->aux().catch_var, s->tok.line, s->tok.col);
                for (const auto& bs : s->aux().then_body) compile_stmt(fn, bs, func_indices);
                fn.code[jmp_end_pos].arg = (int)fn.code.size();
                break;
            }
            case StmtKind::While: {
                int cond_pos = (int)fn.code.size();
                compile_expr(fn, s->cond, func_indices);
                int jmpf_pos = (int)fn.code.size();
                emit(fn, Op::JMPF, 0, s->cond->tok.line, s->cond->tok.col);
                loop_stack_.push_back(make_loop_ctx());
                for (const auto& bs : s->body) compile_stmt(fn, bs, func_indices);
                LoopCtx ctx = loop_stack_.back();
                loop_stack_.pop_back();
                for (int p : ctx.continue_patches) fn.code[p].arg = cond_pos;
                emit(fn, Op::JMP, cond_pos, s->tok.line, s->tok.col);
                fn.code[jmpf_pos].arg = (int)fn.code.size();
                for (int p : ctx.break_patches) fn.code[p].arg = (int)fn.code.size();
                break;
            }
            case StmtKind::For: {
                bool is_main = (fn.name == "<main>");
                auto load_var = [&](const std::string& name) {
                    if (is_main) {
                        int g = ir_.add_global(name);
                        emit(fn, Op::LOAD_GLOBAL, g, s->tok.line, s->tok.col);
                    } else {
                        int slot = alloc_local(fn, name);
                        if (fn.boxed_locals.count(name)) {
                            emit(fn, Op::LOAD_LOCAL_BOXED, slot, s->tok.line, s->tok.col);
                        } else {
                            emit(fn, Op::LOAD_LOCAL, slot, s->tok.line, s->tok.col);
                        }
                    }
                };
                auto store_var = [&](const std::string& name) {
                    if (is_main) {
                        int g = ir_.add_global(name);
                        emit(fn, Op::STORE_GLOBAL, g, s->tok.line, s->tok.col);
                    } else {
                        int slot = alloc_local(fn, name);
                        if (fn.boxed_locals.count(name)) {
                            emit(fn, Op::STORE_LOCAL_BOXED, slot, s->tok.line, s->tok.col);
                        } else {
                            emit(fn, Op::STORE_LOCAL, slot, s->tok.line, s->tok.col);
                        }
                    }
                };
                std::string iter_name = "$iter_" + s->aux().loop_var;
                std::string idx_name = "$idx_" + s->aux().loop_var;
                const bool lazy_range = is_lazy_range(fn, s->aux().iterable, func_indices);
                if (lazy_range) {
                    compile_expr(fn, s->aux().iterable->kids().args[0], func_indices);
                    emit(fn, Op::CALL_NATIVE,
                         pack_native_call(ir_.builtin_index.at("__range_bound"), 1, s->tok),
                         s->tok.line, s->tok.col);
                } else {
                    compile_expr(fn, s->aux().iterable, func_indices);
                    if (ir_.builtin_index.count("__iter_source") > 0) {
                        emit(fn, Op::CALL_NATIVE,
                             pack_native_call(ir_.builtin_index.at("__iter_source"), 1, s->tok),
                             s->tok.line, s->tok.col);
                    }
                }
                store_var(iter_name);
                emit(fn, Op::PUSH_INT, ir_.add_int(0), s->tok.line, s->tok.col);
                store_var(idx_name);
                int cond_pos = (int)fn.code.size();
                load_var(idx_name);
                load_var(iter_name);
                if (!lazy_range) {
                    emit(fn, Op::LEN, 0, s->tok.line, s->tok.col);
                }
                emit(fn, Op::LT, 0, s->tok.line, s->tok.col);
                int jmpf_pos = (int)fn.code.size();
                emit(fn, Op::JMPF, 0, s->tok.line, s->tok.col);
                if (lazy_range) {
                    load_var(idx_name);
                } else {
                    load_var(iter_name);
                    load_var(idx_name);
                    emit(fn, Op::GET_INDEX, 0, s->tok.line, s->tok.col);
                }
                store_var(s->aux().loop_var);
                loop_stack_.push_back(make_loop_ctx());
                for (const auto& bs : s->body) compile_stmt(fn, bs, func_indices);
                LoopCtx ctx = loop_stack_.back();
                loop_stack_.pop_back();
                int incr_pos = (int)fn.code.size();
                for (int p : ctx.continue_patches) fn.code[p].arg = incr_pos;
                load_var(idx_name);
                emit(fn, Op::PUSH_INT, ir_.add_int(1), s->tok.line, s->tok.col);
                emit(fn, Op::ADD, 0, s->tok.line, s->tok.col);
                store_var(idx_name);
                emit(fn, Op::JMP, cond_pos, s->tok.line, s->tok.col);
                fn.code[jmpf_pos].arg = (int)fn.code.size();
                for (int p : ctx.break_patches) fn.code[p].arg = (int)fn.code.size();
                break;
            }
            case StmtKind::Break: {
                if (loop_stack_.empty()) break;
                emit_try_exits_for_jump(fn, s->tok.line, s->tok.col);
                int p = (int)fn.code.size();
                emit(fn, Op::JMP, 0, s->tok.line, s->tok.col);
                loop_stack_.back().break_patches.push_back(p);
                break;
            }
            case StmtKind::Continue: {
                if (loop_stack_.empty()) break;
                emit_try_exits_for_jump(fn, s->tok.line, s->tok.col);
                int p = (int)fn.code.size();
                emit(fn, Op::JMP, 0, s->tok.line, s->tok.col);
                loop_stack_.back().continue_patches.push_back(p);
                break;
            }
            case StmtKind::Return:
                if (s->ret_value) {
                    compile_expr(fn, s->ret_value, func_indices);
                } else {
                    emit(fn, Op::PUSH_NONE, 0, s->tok.line, s->tok.col);
                }
                emit(fn, Op::RET, 0, s->tok.line, s->tok.col);
                break;
            case StmtKind::Block:
                for (const auto& bs : s->body) compile_stmt(fn, bs, func_indices);
                break;
        }
    }

    bool is_output_expr(const ExprPtr& e) {
        if (!e || e->kind != ExprKind::Binary || e->op != TokenType::GT) return false;
        if (e->kids().left && e->kids().left->kind == ExprKind::Member && e->kids().left->name == "out") {
            return true;
        }
        if (e->kids().left && e->kids().left->kind == ExprKind::Ident) {
            const std::string& name = e->kids().left->name;
            for (const auto& p : using_aliases_) {
                if (p.first == name && p.second == "out") return true;
            }
        }
        if (e->kids().left && is_output_expr(e->kids().left)) return true;
        return false;
    }

    void compile_output_chain(IRFunction& fn, const ExprPtr& e,
                               const std::unordered_map<std::string, int>& func_indices,
                               bool is_outermost) {
        if (e->kids().left->kind == ExprKind::Binary && e->kids().left->op == TokenType::GT &&
            is_output_expr(e->kids().left)) {
            compile_output_chain(fn, e->kids().left, func_indices, false);
        } else {
            emit(fn, Op::PUSH_SINK, ir_.add_str("out"), e->tok.line, e->tok.col);
        }
        compile_expr(fn, e->kids().right, func_indices);
        emit(fn, Op::OUTPUT, is_outermost ? 1 : 0, e->tok.line, e->tok.col);
    }

    void compile_expr(IRFunction& fn, const ExprPtr& e,
                       const std::unordered_map<std::string, int>& func_indices) {
        if (!e) { emit(fn, Op::PUSH_NONE, 0, 0, 0); return; }
        bool was_stmt_top = at_stmt_top_;
        at_stmt_top_ = false;
        switch (e->kind) {
            case ExprKind::Int:
                emit(fn, Op::PUSH_INT, ir_.add_int(e->int_val), e->tok.line, e->tok.col);
                break;
            case ExprKind::Float:
                emit(fn, Op::PUSH_FLOAT, ir_.add_float(e->float_val), e->tok.line, e->tok.col);
                break;
            case ExprKind::String:
                emit(fn, Op::PUSH_STR, ir_.add_str(e->str_val), e->tok.line, e->tok.col);
                break;
            case ExprKind::Bool:
                emit(fn, Op::PUSH_BOOL, e->bool_val ? 1 : 0, e->tok.line, e->tok.col);
                break;
            case ExprKind::None:
                emit(fn, Op::PUSH_NONE, 0, e->tok.line, e->tok.col);
                break;
            case ExprKind::Ident: {
                auto lit = fn.local_slots.find(e->name);
                if (lit != fn.local_slots.end()) {
                    if (fn.boxed_locals.count(e->name)) {
                        emit(fn, Op::LOAD_LOCAL_BOXED, lit->second, e->tok.line, e->tok.col);
                    } else {
                        emit(fn, Op::LOAD_LOCAL, lit->second, e->tok.line, e->tok.col);
                    }
                    break;
                }
                if (try_load_self_or_upvalue(fn, e->name, e->tok.line, e->tok.col)) {
                    break;
                }
                auto fit = func_indices.find(e->name);
                if (fit != func_indices.end()) {
                    emit(fn, Op::PUSH_FUNC, fit->second, e->tok.line, e->tok.col);
                    break;
                }
                auto git = ir_.global_index.find(e->name);
                if (git != ir_.global_index.end()) {
                    emit(fn, Op::LOAD_GLOBAL, git->second, e->tok.line, e->tok.col);
                    break;
                }
                int g = ir_.add_global(e->name);
                emit(fn, Op::LOAD_GLOBAL, g, e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::Binary: {
                if (e->op == TokenType::GT && was_stmt_top && is_output_expr(e)) {
                    compile_output_chain(fn, e, func_indices, true);
                    break;
                }
                if (e->op == TokenType::AND) {
                    compile_expr(fn, e->kids().left, func_indices);
                    int jmpr = (int)fn.code.size();
                    emit(fn, Op::JMPR, 0, e->tok.line, e->tok.col);
                    emit(fn, Op::PUSH_BOOL, 0, e->tok.line, e->tok.col);
                    int jend = (int)fn.code.size();
                    emit(fn, Op::JMP, 0, e->tok.line, e->tok.col);
                    fn.code[jmpr].arg = (int)fn.code.size();
                    emit(fn, Op::POP, 0, e->tok.line, e->tok.col);
                    compile_expr(fn, e->kids().right, func_indices);
                    fn.code[jend].arg = (int)fn.code.size();
                    break;
                }
                if (e->op == TokenType::OR) {
                    compile_expr(fn, e->kids().left, func_indices);
                    int jmpr = (int)fn.code.size();
                    emit(fn, Op::JMPR, 0, e->tok.line, e->tok.col);
                    compile_expr(fn, e->kids().right, func_indices);
                    int jend = (int)fn.code.size();
                    emit(fn, Op::JMP, 0, e->tok.line, e->tok.col);
                    fn.code[jmpr].arg = (int)fn.code.size();
                    fn.code[jend].arg = (int)fn.code.size();
                    break;
                }
                compile_expr(fn, e->kids().left, func_indices);
                compile_expr(fn, e->kids().right, func_indices);
                Op op = Op::ADD;
                switch (e->op) {
                    case TokenType::PLUS: op = Op::ADD; break;
                    case TokenType::MINUS: op = Op::SUB; break;
                    case TokenType::STAR: op = Op::MUL; break;
                    case TokenType::SLASH: op = Op::DIV; break;
                    case TokenType::PERCENT: op = Op::MOD; break;
                    case TokenType::EQ: op = Op::EQ; break;
                    case TokenType::NEQ: op = Op::NEQ; break;
                    case TokenType::LT: op = Op::LT; break;
                    case TokenType::GT: op = Op::GT; break;
                    case TokenType::LE: op = Op::LE; break;
                    case TokenType::GE: op = Op::GE; break;
                    default: break;
                }
                emit(fn, op, 0, e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::Unary: {
                compile_expr(fn, e->kids().operand, func_indices);
                if (e->op == TokenType::MINUS) emit(fn, Op::NEG, 0, e->tok.line, e->tok.col);
                else if (e->op == TokenType::NOT) emit(fn, Op::NOT, 0, e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::Assign: {
                compile_expr(fn, e->kids().value, func_indices);
                if (e->kids().target && e->kids().target->kind == ExprKind::Ident) {
                    store_to_ident(fn, e->kids().target->name, e->tok.line, e->tok.col);
                } else if (e->kids().target && e->kids().target->kind == ExprKind::Member &&
                           e->kids().target->kids().object && e->kids().target->kids().object->kind == ExprKind::Ident &&
                           connected_aliases_.count(e->kids().target->kids().object->name) &&
                           connected_aliases_[e->kids().target->kids().object->name].count(e->kids().target->name)) {
                    int g = ir_.add_global(e->kids().target->name);
                    emit(fn, Op::STORE_GLOBAL, g, e->tok.line, e->tok.col);
                }
                break;
            }
            case ExprKind::CompoundAssign: {
                if (!e->kids().target) break;
                Op op = Op::ADD;
                switch (e->op) {
                    case TokenType::MINUS_EQ: op = Op::SUB; break;
                    case TokenType::STAR_EQ: op = Op::MUL; break;
                    case TokenType::SLASH_EQ: op = Op::DIV; break;
                    case TokenType::PERCENT_EQ: op = Op::MOD; break;
                    default: break;
                }
                ExprPtr target = e->kids().target;
                if (target->kind == ExprKind::Member && target->kids().object &&
                    target->kids().object->kind == ExprKind::Ident &&
                    connected_aliases_.count(target->kids().object->name) &&
                    connected_aliases_[target->kids().object->name].count(target->name)) {
                    int g = ir_.add_global(target->name);
                    emit(fn, Op::LOAD_GLOBAL, g, e->tok.line, e->tok.col);
                    compile_expr(fn, e->kids().value, func_indices);
                    emit(fn, op, 0, e->tok.line, e->tok.col);
                    emit(fn, Op::STORE_GLOBAL, g, e->tok.line, e->tok.col);
                    break;
                }
                if (target->kind == ExprKind::Member && !is_module_alias(target->kids().object)) {
                    target = member_as_index(target);
                }
                if (target->kind == ExprKind::Ident) {
                    const std::string& name = target->name;
                    auto lit = fn.local_slots.find(name);
                    if (lit != fn.local_slots.end()) {
                        if (fn.boxed_locals.count(name)) {
                            emit(fn, Op::LOAD_LOCAL_BOXED, lit->second, e->tok.line, e->tok.col);
                        } else {
                            emit(fn, Op::LOAD_LOCAL, lit->second, e->tok.line, e->tok.col);
                        }
                    } else if (!try_load_self_or_upvalue(fn, name, e->tok.line, e->tok.col)) {
                        int g = ir_.add_global(name);
                        emit(fn, Op::LOAD_GLOBAL, g, e->tok.line, e->tok.col);
                    }
                    compile_expr(fn, e->kids().value, func_indices);
                    emit(fn, op, 0, e->tok.line, e->tok.col);
                    store_to_ident(fn, name, e->tok.line, e->tok.col);
                } else if (target->kind == ExprKind::Index) {
                    compile_expr(fn, target->kids().object, func_indices);
                    compile_expr(fn, target->kids().index, func_indices);
                    emit(fn, Op::GET_INDEX, 0, e->tok.line, e->tok.col);
                    compile_expr(fn, e->kids().value, func_indices);
                    emit(fn, op, 0, e->tok.line, e->tok.col);
                    int tmp = alloc_local(fn, "$compound_tmp" + std::to_string(++compound_tmp_counter_));
                    emit(fn, Op::STORE_LOCAL, tmp, e->tok.line, e->tok.col);
                    compile_expr(fn, target->kids().object, func_indices);
                    compile_expr(fn, target->kids().index, func_indices);
                    emit(fn, Op::LOAD_LOCAL, tmp, e->tok.line, e->tok.col);
                    emit(fn, Op::SET_INDEX, 0, e->tok.line, e->tok.col);
                } else {
                    break;
                }
                break;
            }
            case ExprKind::Call: {
                if (e->kids().callee->kind == ExprKind::Member) {
                    if (e->kids().callee->kids().object && e->kids().callee->kids().object->kind == ExprKind::Ident &&
                        connected_aliases_.count(e->kids().callee->kids().object->name) &&
                        connected_aliases_[e->kids().callee->kids().object->name].count(e->kids().callee->name)) {
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        auto fit = func_indices.find(e->kids().callee->name);
                        if (fit != func_indices.end()) {
                            emit(fn, Op::PUSH_FUNC, fit->second, e->tok.line, e->tok.col);
                            emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                        } else {
                            int g = ir_.add_global(e->kids().callee->name);
                            emit(fn, Op::LOAD_GLOBAL, g, e->tok.line, e->tok.col);
                            emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                        }
                        break;
                    }
                    if (is_module_alias(e->kids().callee->kids().object)) {
                        std::string bname = builtin_key_for(e->kids().callee->kids().object, e->kids().callee->name);
                        auto bit = ir_.builtin_index.find(bname);
                        if (bit == ir_.builtin_index.end()) {
                            ir_.add_builtin(bname);
                            bit = ir_.builtin_index.find(bname);
                        }
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        const bool process_call = needs_process_site(bname);
                        if (process_call) emit_process_site(fn, e);
                        if (!emit_inline_builtin(fn, bname, e->kids().args.size(), e->tok)) {
                            const size_t nargs = e->kids().args.size() + (process_call ? 3 : 0);
                            int packed = pack_native_call(bit->second, nargs, e->tok);
                            emit(fn, Op::CALL_NATIVE, packed, e->tok.line, e->tok.col);
                        }
                        break;
                    }
                    int tmp = alloc_local(fn, "$self" + std::to_string(++compound_tmp_counter_));
                    compile_expr(fn, e->kids().callee->kids().object, func_indices);
                    emit(fn, Op::STORE_LOCAL, tmp, e->tok.line, e->tok.col);
                    emit(fn, Op::LOAD_LOCAL, tmp, e->tok.line, e->tok.col);
                    for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                    emit(fn, Op::LOAD_LOCAL, tmp, e->tok.line, e->tok.col);
                    emit(fn, Op::PUSH_STR, ir_.add_str(e->kids().callee->name), e->tok.line, e->tok.col);
                    emit(fn, Op::GET_INDEX, 0, e->tok.line, e->tok.col);
                    emit(fn, Op::CALL, (int)e->kids().args.size() + 1, e->tok.line, e->tok.col);
                    break;
                }
                if (e->kids().callee->kind == ExprKind::Ident) {
                    const std::string& name = e->kids().callee->name;
                    auto lit = fn.local_slots.find(name);
                    bool is_self_or_upvalue = (!self_name_stack_.empty() && self_name_stack_.back() == name) ||
                                               fn.upvalue_slots.count(name) > 0;
                    if (lit != fn.local_slots.end() || is_self_or_upvalue) {
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        if (lit != fn.local_slots.end()) {
                            if (fn.boxed_locals.count(name)) {
                                emit(fn, Op::LOAD_LOCAL_BOXED, lit->second, e->tok.line, e->tok.col);
                            } else {
                                emit(fn, Op::LOAD_LOCAL, lit->second, e->tok.line, e->tok.col);
                            }
                        } else {
                            try_load_self_or_upvalue(fn, name, e->tok.line, e->tok.col);
                        }
                        emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                        break;
                    }
                    auto fit = func_indices.find(name);
                    if (fit != func_indices.end()) {
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        emit(fn, Op::PUSH_FUNC, fit->second, e->tok.line, e->tok.col);
                        emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                        break;
                    }
                    auto bit = ir_.builtin_index.find(name);
                    if (bit != ir_.builtin_index.end()) {
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        if (!emit_inline_builtin(fn, name, e->kids().args.size(), e->tok)) {
                            int packed = pack_native_call(bit->second, e->kids().args.size(), e->tok);
                            emit(fn, Op::CALL_NATIVE, packed, e->tok.line, e->tok.col);
                        }
                        break;
                    }
                    auto git = ir_.global_index.find(name);
                    if (git != ir_.global_index.end()) {
                        for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                        emit(fn, Op::LOAD_GLOBAL, git->second, e->tok.line, e->tok.col);
                        emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                        break;
                    }
                    for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                    emit(fn, Op::PUSH_NONE, 0, e->tok.line, e->tok.col);
                } else {
                    for (const auto& a : e->kids().args) compile_expr(fn, a, func_indices);
                    compile_expr(fn, e->kids().callee, func_indices);
                    emit(fn, Op::CALL, (int)e->kids().args.size(), e->tok.line, e->tok.col);
                }
                break;
            }
            case ExprKind::Member: {
                if (e->kids().object && e->kids().object->kind == ExprKind::Ident &&
                    connected_aliases_.count(e->kids().object->name) &&
                    connected_aliases_[e->kids().object->name].count(e->name)) {
                    auto fit = func_indices.find(e->name);
                    if (fit != func_indices.end()) emit(fn, Op::PUSH_FUNC, fit->second, e->tok.line, e->tok.col);
                    else emit(fn, Op::LOAD_GLOBAL, ir_.add_global(e->name), e->tok.line, e->tok.col);
                    break;
                }
                if (is_module_alias(e->kids().object)) {
                    if (e->name == "out") {
                        emit(fn, Op::PUSH_SINK, ir_.add_str("out"), e->tok.line, e->tok.col);
                    } else {
                        std::string bname = builtin_key_for(e->kids().object, e->name);
                        auto bit = ir_.builtin_index.find(bname);
                        if (bit == ir_.builtin_index.end()) {
                            ir_.add_builtin(bname);
                            bit = ir_.builtin_index.find(bname);
                        }
                        emit(fn, Op::PUSH_BUILTIN, bit->second, e->tok.line, e->tok.col);
                    }
                    break;
                }
                compile_expr(fn, e->kids().object, func_indices);
                emit(fn, Op::PUSH_STR, ir_.add_str(e->name), e->tok.line, e->tok.col);
                emit(fn, Op::GET_INDEX, 0, e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::Index: {
                compile_expr(fn, e->kids().object, func_indices);
                compile_expr(fn, e->kids().index, func_indices);
                emit(fn, Op::GET_INDEX, 0, e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::VectorLiteral: {
                for (const auto& el : e->kids().elements) compile_expr(fn, el, func_indices);
                emit(fn, Op::MAKE_VEC, (int)e->kids().elements.size(), e->tok.line, e->tok.col);
                break;
            }
            case ExprKind::DictLiteral: {
                for (const auto& kv : e->kids().dict_pairs) {
                    compile_expr(fn, kv.first, func_indices);
                    compile_expr(fn, kv.second, func_indices);
                }
                emit(fn, Op::MAKE_DICT, (int)e->kids().dict_pairs.size(), e->tok.line, e->tok.col);
                break;
            }
        }
    }
};

CodegenResult generate(const Module& ast,
                       const std::vector<std::pair<std::string,std::string>>& using_aliases,
                       const std::string& source,
                       const std::vector<std::string>& builtin_names) {
    Codegen c(using_aliases, source, builtin_names);
    return c.run(ast);
}

}
