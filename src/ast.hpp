#pragma once

#include "lexer.hpp"
#include <memory>
#include <string>
#include <vector>
#include <variant>

namespace csy {

// fwd
struct Expr;
struct Stmt;
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

struct Expr {
    ExprKind kind;
    NodeTok tok;
    uint32_t depth = 1;

    long long int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    bool bool_val = false;

    std::string name;

    // bin
    TokenType op = TokenType::END_OF_FILE;
    ExprPtr left;
    ExprPtr right;
    ExprPtr operand;

    // assign and compound assign
    ExprPtr target;
    ExprPtr value;

    ExprPtr callee;
    std::vector<ExprPtr> args;

    // Member
    ExprPtr object;

    ExprPtr index;

    // vector lit
    std::vector<ExprPtr> elements;

    // dict lit
    std::vector<std::pair<ExprPtr, ExprPtr>> dict_pairs;
};

inline ExprPtr make_expr(ExprKind k, const NodeTok& t) {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    e->tok = t;
    return e;
}


enum class StmtKind {
    Import, // #inc <mod> as <als>
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

struct Stmt {
    StmtKind kind;
    NodeTok tok;

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

    ExprPtr expr;

    std::string func_name;
    std::vector<std::string> params;
    std::vector<StmtPtr> body;

    ExprPtr cond;
    std::vector<StmtPtr> then_body;
    std::vector<std::pair<ExprPtr, std::vector<StmtPtr>>> elifs;
    std::vector<StmtPtr> else_body;

    std::string loop_var;
    ExprPtr iterable;

    ExprPtr ret_value;

    std::string catch_var;
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
