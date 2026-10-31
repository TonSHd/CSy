#include "ast_printer.hpp"

#include <sstream>

namespace csy {

static void print_expr(const ExprPtr& e, std::stringstream& ss, int indent);
static void print_stmt(const StmtPtr& s, std::stringstream& ss, int indent);

static std::string pad(int n) {
    return std::string(n * 2, ' ');
}

static std::string op_str(TokenType t) {
    return token_type_name(t);
}

static void print_expr(const ExprPtr& e, std::stringstream& ss, int indent) {
    if (!e) { ss << pad(indent) << "<null>\n"; return; }
    switch (e->kind) {
        case ExprKind::Int:
            ss << pad(indent) << "Int(" << e->int_val << ")\n";
            break;
        case ExprKind::Float:
            ss << pad(indent) << "Float(" << e->float_val << ")\n";
            break;
        case ExprKind::String:
            ss << pad(indent) << "String(\"" << e->str_val << "\")\n";
            break;
        case ExprKind::Bool:
            ss << pad(indent) << "Bool(" << (e->bool_val ? "true" : "false") << ")\n";
            break;
        case ExprKind::None:
            ss << pad(indent) << "None\n";
            break;
        case ExprKind::Ident:
            ss << pad(indent) << "Ident(" << e->name << ")\n";
            break;
        case ExprKind::Binary:
            ss << pad(indent) << "Binary(" << op_str(e->op) << ")\n";
            print_expr(e->left, ss, indent + 1);
            print_expr(e->right, ss, indent + 1);
            break;
        case ExprKind::Unary:
            ss << pad(indent) << "Unary(" << op_str(e->op) << ")\n";
            print_expr(e->operand, ss, indent + 1);
            break;
        case ExprKind::Assign:
            ss << pad(indent) << "Assign\n";
            print_expr(e->target, ss, indent + 1);
            print_expr(e->value, ss, indent + 1);
            break;
        case ExprKind::CompoundAssign:
            ss << pad(indent) << "CompoundAssign(" << op_str(e->op) << ")\n";
            print_expr(e->target, ss, indent + 1);
            print_expr(e->value, ss, indent + 1);
            break;
        case ExprKind::Call:
            ss << pad(indent) << "Call\n";
            print_expr(e->callee, ss, indent + 1);
            for (const auto& a : e->args) print_expr(a, ss, indent + 1);
            break;
        case ExprKind::Member:
            ss << pad(indent) << "Member(." << e->name << ")\n";
            print_expr(e->object, ss, indent + 1);
            break;
        case ExprKind::Index:
            ss << pad(indent) << "Index\n";
            print_expr(e->object, ss, indent + 1);
            print_expr(e->index, ss, indent + 1);
            break;
        case ExprKind::DictLiteral:
            ss << pad(indent) << "DictLiteral(" << e->dict_pairs.size() << " pair(s))\n";
            for (const auto& kv : e->dict_pairs) {
                print_expr(kv.first, ss, indent + 1);
                print_expr(kv.second, ss, indent + 1);
            }
            break;
        case ExprKind::VectorLiteral:
            ss << pad(indent) << "Vector\n";
            for (const auto& el : e->elements) print_expr(el, ss, indent + 1);
            break;
    }
}

static void print_body(const std::vector<StmtPtr>& body, std::stringstream& ss, int indent) {
    for (const auto& s : body) print_stmt(s, ss, indent);
}

static void print_stmt(const StmtPtr& s, std::stringstream& ss, int indent) {
    if (!s) { ss << pad(indent) << "<null stmt>\n"; return; }
    switch (s->kind) {
        case StmtKind::Break:
            ss << pad(indent) << "Break\n";
            break;
        case StmtKind::Continue:
            ss << pad(indent) << "Continue\n";
            break;
        case StmtKind::Try:
            ss << pad(indent) << "Try(catch=" << s->catch_var << ")\n";
            print_body(s->body, ss, indent + 1);
            ss << pad(indent) << "Catch\n";
            print_body(s->then_body, ss, indent + 1);
            break;
        case StmtKind::Import:
            ss << pad(indent) << "Import(module=" << s->import_module
               << ", alias=" << s->import_alias << ")\n";
            break;
        case StmtKind::Connect:
            ss << pad(indent) << "Connect(path=" << s->connect_path
               << ", alias=" << s->connect_alias << (s->connect_all ? ", all" : "") << ", exports=";
            for (size_t i = 0; i < s->connect_exports.size(); ++i) {
                if (i) ss << ", ";
                ss << s->connect_exports[i];
            }
            ss << ")\n";
            break;
        case StmtKind::DirectiveAlias:
            ss << pad(indent) << "DirectiveAlias(" << s->alias_directive << " as " << s->using_alias << ")\n";
            break;
        case StmtKind::Explain:
            ss << pad(indent) << "Explain(name=" << s->explain_name << ", refs=" << s->explain_refs.size() << ")\n";
            break;
        case StmtKind::Using:
            ss << pad(indent) << "Using(as=" << s->using_alias << ")\n";
            print_expr(s->using_expr, ss, indent + 1);
            break;
        case StmtKind::VarDecl:
            ss << pad(indent) << "VarDecl\n";
            print_expr(s->expr, ss, indent + 1);
            break;
        case StmtKind::ExprStmt:
            ss << pad(indent) << "ExprStmt\n";
            print_expr(s->expr, ss, indent + 1);
            break;
        case StmtKind::FuncDecl:
            ss << pad(indent) << "Func(" << s->func_name << ", params=";
            for (size_t i = 0; i < s->params.size(); i++) {
                if (i) ss << ", ";
                ss << s->params[i];
            }
            ss << ")\n";
            print_body(s->body, ss, indent + 1);
            break;
        case StmtKind::If:
            ss << pad(indent) << "If\n";
            ss << pad(indent+1) << "Cond:\n";
            print_expr(s->cond, ss, indent + 2);
            ss << pad(indent+1) << "Then:\n";
            print_body(s->then_body, ss, indent + 2);
            for (const auto& el : s->elifs) {
                ss << pad(indent+1) << "Elif:\n";
                print_expr(el.first, ss, indent + 2);
                print_body(el.second, ss, indent + 2);
            }
            if (!s->else_body.empty()) {
                ss << pad(indent+1) << "Else:\n";
                print_body(s->else_body, ss, indent + 2);
            }
            break;
        case StmtKind::While:
            ss << pad(indent) << "While\n";
            print_expr(s->cond, ss, indent + 1);
            print_body(s->body, ss, indent + 1);
            break;
        case StmtKind::For:
            ss << pad(indent) << "For(" << s->loop_var << " in ...)\n";
            print_expr(s->iterable, ss, indent + 1);
            print_body(s->body, ss, indent + 1);
            break;
        case StmtKind::Return:
            ss << pad(indent) << "Return\n";
            if (s->ret_value) print_expr(s->ret_value, ss, indent + 1);
            else ss << pad(indent+1) << "<void>\n";
            break;
        case StmtKind::Block:
            ss << pad(indent) << "Block\n";
            print_body(s->body, ss, indent + 1);
            break;
    }
}

std::string print_ast(const Module& mod) {
    std::stringstream ss;
    ss << "Module(file=" << mod.file << ")\n";
    print_body(mod.body, ss, 1);
    return ss.str();
}

}

