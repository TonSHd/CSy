#pragma once

#include "lexer.hpp"

#include <memory>
#include <string>
#include <vector>
#include <variant>

namespace csy {

struct Expr;
struct Stmt;

template <class T>
class LazyBox {
public:
    LazyBox() = default;
    LazyBox(const LazyBox& other) : ptr_(other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr) {}
    LazyBox& operator=(const LazyBox& other) {
        if (this != &other) ptr_ = other.ptr_ ? std::make_unique<T>(*other.ptr_) : nullptr;
        return *this;
    }
    LazyBox(LazyBox&&) noexcept = default;
    LazyBox& operator=(LazyBox&&) noexcept = default;

    T& get() {
        if (!ptr_) ptr_ = std::make_unique<T>();
        return *ptr_;
    }
    const T& get() const {
        if (ptr_) return *ptr_;
        static const T empty{};
        return empty;
    }

private:
    std::unique_ptr<T> ptr_;
};

using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

enum class ExprKind {
    Int, Float, String, Bool, None,
    Ident,
    Binary, Unary,
    Assign,
    CompoundAssign,
    Call,
    Member,
    Index,
    VectorLiteral,
    DictLiteral,
};

constexpr uint32_t kMaxExprTreeDepth = 1000;

struct ExprKids {
    ExprPtr left;
    ExprPtr right;
    ExprPtr operand;

    ExprPtr target;
    ExprPtr value;

    ExprPtr callee;
    std::vector<ExprPtr> args;

    ExprPtr object;

    ExprPtr index;

    std::vector<ExprPtr> elements;

    std::vector<std::pair<ExprPtr, ExprPtr>> dict_pairs;
};

struct ExprAux {
    std::string native_fallback_func;
    std::string process_project_root;
    std::string process_site_key;
    std::string process_site_location;
};

struct Expr {
    ExprKind kind;
    NodeTok tok;
    uint32_t depth = 1;

    long long int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;

    std::string name;

    TokenType op = TokenType::END_OF_FILE;

    const ExprKids& kids() const { return kids_.get(); }
    ExprKids& kids_mut() { return kids_.get(); }
    const ExprAux& aux() const { return aux_.get(); }
    ExprAux& aux_mut() { return aux_.get(); }

private:
    LazyBox<ExprKids> kids_;
    LazyBox<ExprAux> aux_;
};

inline ExprPtr make_expr(ExprKind k, const NodeTok& t) {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    e->tok = t;
    return e;
}


enum class StmtKind {
    Import,
    Connect,
    Explain,
    DirectiveAlias,
    Using,
    VarDecl,
    ExprStmt,
    FuncDecl,
    If,
    While,
    For,
    Return,
    Break,
    Continue,
    Block,
    Try,
};

struct StmtAux {
    std::string import_module;
    std::string import_alias;

    std::string connect_path;
    std::string connect_alias;
    std::vector<std::string> connect_exports;
    bool connect_all = false;

    std::string explain_name;
    std::string explain_text;
    std::vector<std::string> explain_refs;

    std::string alias_directive;

    ExprPtr using_expr;
    std::string using_alias;

    std::string func_name;
    std::vector<std::string> params;

    std::vector<StmtPtr> then_body;
    std::vector<std::pair<ExprPtr, std::vector<StmtPtr>>> elifs;
    std::vector<StmtPtr> else_body;

    std::string loop_var;
    ExprPtr iterable;

    std::string catch_var;
};

struct Stmt {
    StmtKind kind;
    NodeTok tok;

    ExprPtr expr;

    std::vector<StmtPtr> body;

    ExprPtr cond;

    ExprPtr ret_value;

    const StmtAux& aux() const { return aux_.get(); }
    StmtAux& aux_mut() { return aux_.get(); }

private:
    LazyBox<StmtAux> aux_;
};

inline StmtPtr make_stmt(StmtKind k, const NodeTok& t) {
    auto s = std::make_shared<Stmt>();
    s->kind = k;
    s->tok = t;
    return s;
}

struct LoopOutlineNote {
    int line = 0;
    bool outlined = false;
    std::vector<std::string> fns;
    std::string detail;
    std::vector<bool> scalar_arg;
    std::string sig_hint;
    StmtPtr original;
    std::vector<StmtPtr> call_stmts;
};

struct Module {
    std::string file;
    std::vector<StmtPtr> body;
    std::vector<StmtPtr> explains;
    std::vector<LoopOutlineNote> loop_notes;
};

}
