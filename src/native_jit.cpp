#include "native_jit.hpp"
#include "resource.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <map>
#include <dirent.h>
#include <ctime>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "hash128.hpp"

namespace csy {

namespace {

bool debug_on() { return std::getenv("CSY_JIT_DEBUG") != nullptr; }
void dbg(const std::string& msg) {
    if (debug_on()) fprintf(stderr, "[csy][jit-debug] %s\n", msg.c_str());
}

enum class MathKind { AlwaysFloat1, AlwaysFloat2, AlwaysFloat3, Promote2 };
struct MathBuiltin { MathKind kind; std::string c_expr_template; };

const std::unordered_map<std::string, MathBuiltin>& math_whitelist() {
    static const std::unordered_map<std::string, MathBuiltin> m = {
        {"sin",   {MathKind::AlwaysFloat1, "sin({0})"}},
        {"cos",   {MathKind::AlwaysFloat1, "cos({0})"}},
        {"tan",   {MathKind::AlwaysFloat1, "tan({0})"}},
        {"asin",  {MathKind::AlwaysFloat1, "asin({0})"}},
        {"acos",  {MathKind::AlwaysFloat1, "acos({0})"}},
        {"atan",  {MathKind::AlwaysFloat1, "atan({0})"}},
        {"sqrt",  {MathKind::AlwaysFloat1, "sqrt({0})"}},
        {"exp",   {MathKind::AlwaysFloat1, "exp({0})"}},
        {"log",   {MathKind::AlwaysFloat1, "log({0})"}},
        {"log2",  {MathKind::AlwaysFloat1, "log2({0})"}},
        {"log10", {MathKind::AlwaysFloat1, "log10({0})"}},
        {"floor", {MathKind::AlwaysFloat1, "floor({0})"}},
        {"ceil",  {MathKind::AlwaysFloat1, "ceil({0})"}},
        {"round", {MathKind::AlwaysFloat1, "round({0})"}},
        {"abs",   {MathKind::AlwaysFloat1, "fabs({0})"}},
        {"sign",  {MathKind::AlwaysFloat1, "csy_sign({0})"}},
        {"tanh",  {MathKind::AlwaysFloat1, "tanh({0})"}},
        {"sigmoid",       {MathKind::AlwaysFloat1, "csy_sigmoid({0})"}},
        {"relu",          {MathKind::AlwaysFloat1, "csy_relu({0})"}},
        {"sigmoid_deriv", {MathKind::AlwaysFloat1, "csy_sigmoid_deriv({0})"}},
        {"relu_deriv",    {MathKind::AlwaysFloat1, "csy_relu_deriv({0})"}},
        {"tanh_deriv",    {MathKind::AlwaysFloat1, "csy_tanh_deriv({0})"}},
        {"atan2", {MathKind::AlwaysFloat2, "atan2({0}, {1})"}},
        {"pow",   {MathKind::AlwaysFloat2, "pow({0}, {1})"}},
        {"clamp", {MathKind::AlwaysFloat3, "fmax({1}, fmin({0}, {2}))"}},
        {"max",   {MathKind::Promote2, ""}},
        {"min",   {MathKind::Promote2, ""}},
    };
    return m;
}

const char* kPreamble = R"PRE(
#include <math.h>
#include <limits.h>

typedef struct { double number; long long ival; double* data; long long length; int is_array; } CsyNativeArg;
typedef union { long long i; double f; } CsyRet;

/* range(x) iteration count for a float bound: same coercion as the VM's
   __range_bound builtin (never negative; NaN/huge handled without UB). */
static long long csy_bound_f(double d) {
    if (!(d > 0.0)) return 0;
    if (d >= 9.2e18) return LLONG_MAX;
    return (long long)d;
}

static double csy_array_get(double* data, long long length, long long index, int* err) {
    if (*err) return 0.0;
    if (index < 0) index += length;
    if (index < 0 || index >= length) { if (*err == 0) *err = 3; return 0.0; }
    return data[index];
}
static void csy_array_set(double* data, long long length, long long index, double value, int* err) {
    if (*err) return;
    if (index < 0) index += length;
    if (index < 0 || index >= length) { if (*err == 0) *err = 3; return; }
    data[index] = value;
}
static void csy_array_compound(double* data, long long length, long long index,
                               double value, int subtract, int* err) {
    if (*err) return;
    if (index < 0) index += length;
    if (index < 0 || index >= length) { if (*err == 0) *err = 3; return; }
    data[index] += subtract ? -value : value;
}

static double csy_sign(double x) { return x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0); }
static double csy_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
static double csy_relu(double x) { return x > 0.0 ? x : 0.0; }
static double csy_sigmoid_deriv(double y) { return y * (1.0 - y); }
static double csy_relu_deriv(double y) { return y > 0.0 ? 1.0 : 0.0; }
static double csy_tanh_deriv(double y) { return 1.0 - y * y; }
static long long csy_max_i(long long a, long long b) { return a > b ? a : b; }
static long long csy_min_i(long long a, long long b) { return a < b ? a : b; }
static long long csy_overflow(int* err) { if (*err == 0) *err = 4; return 0; }
static long long csy_safe_add_i(long long a, long long b, int* err) {
    long long r; if (__builtin_add_overflow(a, b, &r)) return csy_overflow(err); return r;
}
static long long csy_safe_sub_i(long long a, long long b, int* err) {
    long long r; if (__builtin_sub_overflow(a, b, &r)) return csy_overflow(err); return r;
}
static long long csy_safe_mul_i(long long a, long long b, int* err) {
    long long r; if (__builtin_mul_overflow(a, b, &r)) return csy_overflow(err); return r;
}
static long long csy_safe_neg_i(long long a, int* err) {
    if (a == LLONG_MIN) return csy_overflow(err); return -a;
}
static long long csy_safe_mod_i(long long a, long long b, int* err) {
    if (b == 0) { if (*err == 0) *err = 1; return 0; }
    if (a == LLONG_MIN && b == -1) return 0;
    return a % b;
}

/* *__err is only ever set, never cleared once set - see native_jit.cpp's
   comment on error propagation: a division-by-zero anywhere in the call
   tree must survive to be reported at the top, even if sibling
   sub-expressions keep computing (wastefully but harmlessly) afterward. */
static double csy_safe_div_f(double a, double b, int* err) {
    if (b == 0.0) { if (*err == 0) *err = 1; return 0.0; }
    return a / b;
}
static long long csy_safe_div_i(long long a, long long b, int* err) {
    if (b == 0) { if (*err == 0) *err = 1; return 0; }
    if (a == LLONG_MIN && b == -1) return csy_overflow(err);
    return a / b;
}

/* Recursion guard: native frames are tiny compared to the interpreter's,
   so this can go much deeper than the interpreter's own limit (3000) and
   still stay safely inside a typical 8MB stack. */
#define CSY_MAX_NATIVE_DEPTH 20000
)PRE";

}

// Written by Grok, Claude

// ============================================================
// Stage 1: eligibility
// ============================================================

enum class NType { Int, Float, Array };

// One FnInfo per *instance*: a function compiled for one specific combination
// of argument types (its "signature"). One letter per parameter:
//   I  int
//   F  float that is a float at run time, always
//   M  float-typed but possibly an int at run time (a variable the program
//      assigns both ints and floats). Only ever used between native functions.
//   A  f64_array
// The same struct also describes a *source* function (the "base"), in which
// case sig/key are empty; instances copy it.
struct FnInfo {
    const Stmt* decl = nullptr;
    std::vector<std::string> param_names;
    size_t user_param_count = 0;
    std::vector<std::string> global_params;  // read-only module globals, appended to the native ABI
    std::unordered_set<std::string> direct_global_reads;
    std::unordered_set<std::string> array_params;
    std::unordered_set<std::string> calls_candidates;  // other candidate fn names called
    bool syntax_ok = false;
    std::string why_syntax;     // why the scanner refused it (for --explain-native)
    bool eligible = false;  // after call-graph closure

    // Instance identity.
    const std::unordered_map<std::string, FnInfo>* bases = nullptr;  // every source function by name
    std::string base_name;
    std::string sig;
    std::string key;   // base_name + "$" + sig
    int id = -1;       // assigned just before C emission

    // Filled in by type inference.
    std::unordered_map<std::string, NType> local_types;  // includes params
    // Which kinds of value each variable is ever assigned: bit 1 = an int,
    // bit 2 = a float. A float-typed variable with bit 1 set may hold an int
    // at run time, exactly like in the interpreter - see definitely_float().
    std::unordered_map<std::string, int> seen;
    NType return_type = NType::Int;
    int ret_seen = 0;   // same bits, for the values this function returns
    bool has_return = false;
    bool valid = true;
    std::string why_invalid;
};
using InstMap = std::unordered_map<std::string, FnInfo>;

constexpr size_t kMaxInstances = 512;
constexpr size_t kMaxNativeParams = 16;

NType infer_expr_type(const InstMap& insts, const FnInfo& fn, const ExprPtr& e);
bool definitely_float(const InstMap& insts, const FnInfo& fn, const ExprPtr& e);

bool returns_maybe_int(const FnInfo& f) {
    return f.return_type == NType::Float && (f.ret_seen & 1) != 0;
}

// Signature letter for one call argument, from the CALLER's point of view.
char arg_sig_char(const InstMap& insts, const FnInfo& caller, const ExprPtr& a) {
    if (infer_expr_type(insts, caller, a) == NType::Int) return 'I';
    return definitely_float(insts, caller, a) ? 'F' : 'M';
}

// Which instance does this call expression invoke? Chosen purely from the
// static types of the arguments in the caller, so the answer is the same
// during inference, validation and C generation.
bool callee_key(const InstMap& insts, const FnInfo& caller, const ExprPtr& call,
                std::string& key_out, std::string* base_out = nullptr, std::string* sig_out = nullptr) {
    if (!call || call->kind != ExprKind::Call || !call->callee || call->callee->kind != ExprKind::Ident) return false;
    if (!caller.bases) return false;
    auto b = caller.bases->find(call->callee->name);
    if (b == caller.bases->end()) return false;
    const FnInfo& base = b->second;
    if (call->args.size() != base.user_param_count) return false;
    std::string sig;
    for (size_t i = 0; i < call->args.size(); i++) {
        if (base.array_params.count(base.param_names[i])) sig += 'A';
        else sig += arg_sig_char(insts, caller, call->args[i]);
    }
    for (const auto& global : base.global_params) {
        auto local = caller.local_types.find(global);
        if (local == caller.local_types.end() || local->second == NType::Array) return false;
        auto ident = std::make_shared<Expr>();
        ident->kind = ExprKind::Ident;
        ident->name = global;
        sig += arg_sig_char(insts, caller, ident);
    }
    key_out = call->callee->name + "$" + sig;
    if (base_out) *base_out = call->callee->name;
    if (sig_out) *sig_out = sig;
    return true;
}

// The single source of truth for "what type does this expression have, given
// the current inferred types for locals/params and other instances' return
// types". Used by inference (where types are still converging), validation and
// C codegen (where they are final) - one implementation, so they cannot drift.
NType infer_expr_type(const InstMap& insts, const FnInfo& fn, const ExprPtr& e) {
    if (!e) return NType::Int;
    switch (e->kind) {
        case ExprKind::Int: return NType::Int;
        case ExprKind::Float: return NType::Float;
        case ExprKind::Ident: {
            auto it = fn.local_types.find(e->name);
            return it != fn.local_types.end() ? it->second : NType::Int;
        }
        case ExprKind::Unary:
            return e->op == TokenType::MINUS ? infer_expr_type(insts, fn, e->operand) : NType::Int;
        case ExprKind::Binary: {
            if (e->op == TokenType::LT || e->op == TokenType::GT || e->op == TokenType::LE ||
                e->op == TokenType::GE || e->op == TokenType::EQ || e->op == TokenType::NEQ ||
                e->op == TokenType::AND || e->op == TokenType::OR) {
                return NType::Int;  // bool-as-int; only reachable in a cond, never stored
            }
            NType l = infer_expr_type(insts, fn, e->left), r = infer_expr_type(insts, fn, e->right);
            return (l == NType::Float || r == NType::Float) ? NType::Float : NType::Int;
        }
        case ExprKind::Index: return NType::Float;
        case ExprKind::Call: {
            const std::string& name = e->callee->name;
            auto mit = math_whitelist().find(name);
            if (mit != math_whitelist().end()) {
                if (mit->second.kind == MathKind::Promote2) {
                    NType a = e->args.size() > 0 ? infer_expr_type(insts, fn, e->args[0]) : NType::Int;
                    NType b = e->args.size() > 1 ? infer_expr_type(insts, fn, e->args[1]) : NType::Int;
                    return (a == NType::Float || b == NType::Float) ? NType::Float : NType::Int;
                }
                return NType::Float;
            }
            std::string key;
            if (callee_key(insts, fn, e, key)) {
                auto fit = insts.find(key);
                if (fit != insts.end()) return fit->second.return_type;
            }
            return NType::Int;
        }
        default:
            return NType::Int;
    }
}

// True when the expression is a float at run time on EVERY path, in the
// interpreter too. The interpreter is dynamically typed: `s = 0; ...; s += 0.5`
// leaves s an int until the float assignment actually runs. The native code
// gives s one static type (float), so the two can differ in the TYPE of such a
// value (never in its numeric value). That only changes behaviour where int and
// float semantics differ - division - so we track precisely which expressions
// are certain to be floats and refuse to compile a division whose result might
// differ (see instance_expressions_ok).
bool definitely_float(const InstMap& insts, const FnInfo& fn, const ExprPtr& e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Float: return true;
        case ExprKind::Ident: {
            auto t = fn.local_types.find(e->name);
            if (t == fn.local_types.end() || t->second != NType::Float) return false;
            auto s = fn.seen.find(e->name);
            return s == fn.seen.end() || (s->second & 1) == 0;
        }
        case ExprKind::Unary:
            return e->op == TokenType::MINUS && definitely_float(insts, fn, e->operand);
        case ExprKind::Binary:
            if (e->op == TokenType::PLUS || e->op == TokenType::MINUS || e->op == TokenType::STAR ||
                e->op == TokenType::SLASH || e->op == TokenType::PERCENT) {
                return definitely_float(insts, fn, e->left) || definitely_float(insts, fn, e->right);
            }
            return false;
        case ExprKind::Index: return true;
        case ExprKind::Call: {
            auto mit = math_whitelist().find(e->callee->name);
            if (mit != math_whitelist().end()) {
                if (mit->second.kind != MathKind::Promote2) return true;
                return (e->args.size() > 0 && definitely_float(insts, fn, e->args[0])) ||
                       (e->args.size() > 1 && definitely_float(insts, fn, e->args[1]));
            }
            std::string key;
            if (callee_key(insts, fn, e, key)) {
                auto fit = insts.find(key);
                if (fit != insts.end())
                    return fit->second.return_type == NType::Float && !returns_maybe_int(fit->second);
            }
            return false;
        }
        default: return false;
    }
}

// Visit every node of an expression tree; false as soon as `pred` rejects one.
bool expr_tree_all(const ExprPtr& e, const std::function<bool(const ExprPtr&)>& pred) {
    if (!e) return true;
    if (!pred(e)) return false;
    if (!expr_tree_all(e->left, pred) || !expr_tree_all(e->right, pred) ||
        !expr_tree_all(e->operand, pred) || !expr_tree_all(e->target, pred) ||
        !expr_tree_all(e->value, pred) || !expr_tree_all(e->callee, pred) ||
        !expr_tree_all(e->object, pred) || !expr_tree_all(e->index, pred)) return false;
    for (const auto& a : e->args) if (!expr_tree_all(a, pred)) return false;
    for (const auto& item : e->elements) if (!expr_tree_all(item, pred)) return false;
    for (const auto& pair : e->dict_pairs)
        if (!expr_tree_all(pair.first, pred) || !expr_tree_all(pair.second, pred)) return false;
    return true;
}

// Apply `pred` (a whole-expression-tree check) to every expression of every
// statement in a body, recursing into nested bodies.
bool all_stmt_exprs(const std::vector<StmtPtr>& body, const std::function<bool(const ExprPtr&)>& check) {
    for (const auto& s : body) {
        if (!s) continue;
        // A `for` header is `range(bound)`; the scanner has already pinned that
        // shape, so only the bound expression is a real expression to examine.
        const ExprPtr& iterable = s->kind == StmtKind::For && s->iterable && !s->iterable->args.empty()
                                      ? s->iterable->args[0] : s->iterable;
        if (!check(s->expr) || !check(s->cond) || !check(iterable) || !check(s->ret_value)) return false;
        if (!all_stmt_exprs(s->body, check) || !all_stmt_exprs(s->then_body, check) ||
            !all_stmt_exprs(s->else_body, check)) return false;
        for (const auto& branch : s->elifs) {
            if (!check(branch.first) || !all_stmt_exprs(branch.second, check)) return false;
        }
    }
    return true;
}

static std::string user_visible_name(const std::string& n) {
    if (n.compare(0, 7, "__csy_u") == 0) {
        const size_t us = n.find('_', 7);
        if (us != std::string::npos && us + 1 < n.size()) return user_visible_name(n.substr(us + 1));
    }
    // private names the loop outliner gives a loop's own copy of a variable: `x__L40`
    const size_t mark = n.rfind("__L");
    if (mark != std::string::npos && mark > 0) {
        size_t e = mark + 3;
        while (e < n.size() && std::isdigit(static_cast<unsigned char>(n[e]))) e++;
        const size_t digits = e - (mark + 3);
        while (e < n.size() && n[e] == '_') e++;
        if (digits > 0 && e == n.size()) return n.substr(0, mark);
    }
    return n;
}

// ---- Move 1b: ints that live in a double -------------------------------------------------
// A variable that is an int on some paths and a float on others ("mixed") is kept as a
// double plus a runtime tag. That is exact for small ints, but the interpreter's ints are
// 64-bit with overflow errors, so native code would silently differ (no overflow error,
// rounding above 2^53) wherever an int-capable value goes through double arithmetic or is
// stored into a double. Rather than guess, such instances stay interpreted. What remains
// native keeps one invariant: every int-tagged double is a small constant or a copy of one.
constexpr long long kExactDoubleInt = 1LL << 53;   // below this every int is exactly a double

bool int_constant_value(const ExprPtr& e, long long& out) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Int:
            out = e->int_val;
            return true;
        case ExprKind::Unary: {
            long long v = 0;
            if (e->op != TokenType::MINUS || !int_constant_value(e->operand, v) || v == LLONG_MIN) return false;
            out = -v;
            return true;
        }
        case ExprKind::Binary: {
            long long a = 0, b = 0;
            if (!int_constant_value(e->left, a) || !int_constant_value(e->right, b)) return false;
            if (e->op == TokenType::PLUS) return !__builtin_add_overflow(a, b, &out);
            if (e->op == TokenType::MINUS) return !__builtin_sub_overflow(a, b, &out);
            if (e->op == TokenType::STAR) return !__builtin_mul_overflow(a, b, &out);
            return false;
        }
        default:
            return false;
    }
}

bool small_int_constant(const ExprPtr& e) {
    long long v = 0;
    return int_constant_value(e, v) && v > -kExactDoubleInt && v < kExactDoubleInt;
}

// Int parameters that the body turns into floats: the native entry converts the int to a double,
// which is only exact below 2^53, so the entry wrapper rejects larger values (error 5, raised before
// the function has done anything; the VM then runs the bytecode version instead).
std::vector<size_t> entry_guard_params(const FnInfo& fn) {
    std::vector<size_t> out;
    for (size_t i = 0; i < fn.param_names.size() && i < fn.sig.size(); i++) {
        auto lt = fn.local_types.find(fn.param_names[i]);
        if (fn.sig[i] == 'I' && lt != fn.local_types.end() && lt->second == NType::Float) out.push_back(i);
    }
    return out;
}

// Statement-level conversions of an int into a double that may later be read as an int.
bool conversions_ok(const InstMap& insts, const FnInfo& fn, const std::vector<StmtPtr>& body, std::string& why) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::For) {
            auto lt = fn.local_types.find(s->loop_var);
            if (lt != fn.local_types.end() && lt->second == NType::Float) {
                why = "uses '" + user_visible_name(s->loop_var) + "' as a loop counter and also as a float; "
                      "native code would hold the counter in a double (use a different name for the float)";
                return false;
            }
        }
        if (s->kind == StmtKind::Return && s->ret_value && fn.return_type == NType::Float &&
            infer_expr_type(insts, fn, s->ret_value) == NType::Int && !small_int_constant(s->ret_value)) {
            why = "returns an int on some paths and a float on others; native code would return the int through "
                  "a double, which is inexact above 2^53 (return a float on every path to enable native code)";
            return false;
        }
        if (!conversions_ok(insts, fn, s->body, why) || !conversions_ok(insts, fn, s->then_body, why) ||
            !conversions_ok(insts, fn, s->else_body, why)) return false;
        for (const auto& el : s->elifs)
            if (!conversions_ok(insts, fn, el.second, why)) return false;
    }
    return true;
}

// Reasons an otherwise-compilable instance must stay interpreted. Each of these
// is a place where native and interpreted results could differ, so the rule is
// "don't compile it" rather than "compile it and hope".
//
//  * `%` - the interpreter only defines it for two ints (anything else is
//    `none`); C's `%`/fmod would compute a real, different answer.
//  * `/` - int/int truncates but float division does not. If the static type
//    says float yet neither operand is certain to be a float at run time, the
//    interpreter might truncate where the native code would not.
//  * array index - must be provably an int.
//  * calls - arity must match the callee.
bool instance_expressions_ok(const InstMap& insts, const FnInfo& fn, std::string& why) {
    auto pred = [&](const ExprPtr& n) -> bool {
        if (n->kind == ExprKind::Binary && n->op == TokenType::PERCENT) {
            if (infer_expr_type(insts, fn, n->left) != NType::Int ||
                infer_expr_type(insts, fn, n->right) != NType::Int) {
                why = "uses '%' on a value that is not provably an int";
                return false;
            }
        }
        if (n->kind == ExprKind::Binary && n->op == TokenType::SLASH) {
            if (infer_expr_type(insts, fn, n) == NType::Float &&
                !definitely_float(insts, fn, n->left) && !definitely_float(insts, fn, n->right)) {
                why = "divides by/with a variable that holds an int on some paths and a float on others "
                      "(the interpreter would do int division there) - initialise it with a float like 0.0 to enable native code";
                return false;
            }
        }
        if (n->kind == ExprKind::Index && infer_expr_type(insts, fn, n->index) != NType::Int) {
            why = "indexes an f64_array with a non-int value";
            return false;
        }
        // Move 1b: + - * where the result may be an int at run time but is computed as a double.
        if (n->kind == ExprKind::Binary &&
            (n->op == TokenType::PLUS || n->op == TokenType::MINUS || n->op == TokenType::STAR) &&
            infer_expr_type(insts, fn, n) == NType::Float && !definitely_float(insts, fn, n)) {
            why = "adds, subtracts or multiplies a variable that holds an int on some paths and a float on others; "
                  "native code would miss an integer overflow the interpreter reports - initialise it with a float "
                  "like 0.0 to enable native code";
            return false;
        }
        if (n->kind == ExprKind::CompoundAssign && n->target && n->target->kind == ExprKind::Ident) {
            auto lt = fn.local_types.find(n->target->name);
            const NType tx = lt != fn.local_types.end() ? lt->second : NType::Int;
            const NType tv = infer_expr_type(insts, fn, n->value);
            if (tx == NType::Float || tv == NType::Float) {
                auto sn = fn.seen.find(n->target->name);
                const bool x_certain = tx == NType::Float && (sn == fn.seen.end() || (sn->second & 1) == 0);
                if (!x_certain && !definitely_float(insts, fn, n->value)) {
                    why = "updates a variable that holds an int on some paths and a float on others with += or -=; "
                          "native code would miss an integer overflow the interpreter reports - initialise it with "
                          "a float like 0.0 to enable native code";
                    return false;
                }
            }
        }
        if (n->kind == ExprKind::Assign && n->target && n->target->kind == ExprKind::Ident) {
            auto lt = fn.local_types.find(n->target->name);
            if (lt != fn.local_types.end() && lt->second == NType::Float &&
                infer_expr_type(insts, fn, n->value) == NType::Int && !small_int_constant(n->value)) {
                why = "stores an int into '" + user_visible_name(n->target->name) + "', which is also used as a float; "
                      "native code would keep the int in a double, which is inexact above 2^53";
                return false;
            }
        }
        if (n->kind == ExprKind::Call && n->callee && n->callee->kind == ExprKind::Ident &&
            !math_whitelist().count(n->callee->name)) {
            // The callee turns one of its int parameters into a float. The VM guards that at the entry, but a
            // native caller would hand it the int directly, so only small constants are allowed there.
            std::string key;
            if (callee_key(insts, fn, n, key)) {
                for (size_t gi : entry_guard_params(insts.at(key)))
                    if (gi < n->args.size() && !small_int_constant(n->args[gi])) {
                        why = "passes an int to a function that turns that parameter into a float; native code would "
                              "convert it to a double, which is inexact above 2^53";
                        return false;
                    }
            }
        }
        if (n->kind == ExprKind::Call && n->callee && n->callee->kind == ExprKind::Ident) {
            auto mit = math_whitelist().find(n->callee->name);
            if (mit != math_whitelist().end() && mit->second.kind == MathKind::Promote2 &&
                infer_expr_type(insts, fn, n) == NType::Float && !definitely_float(insts, fn, n)) {
                for (const auto& arg : n->args)
                    if (infer_expr_type(insts, fn, arg) == NType::Int && !small_int_constant(arg)) {
                        why = "mixes an int into max/min with a variable that holds an int on some paths and a float "
                              "on others; native code would hold the int in a double, which is inexact above 2^53";
                        return false;
                    }
            }
        }
        if (n->kind == ExprKind::Call && n->callee && n->callee->kind == ExprKind::Ident &&
            math_whitelist().count(n->callee->name) == 0) {
            std::string key;
            if (!callee_key(insts, fn, n, key)) { why = "calls a function with the wrong number of arguments"; return false; }
        }
        return true;
    };
    if (!all_stmt_exprs(fn.decl->body, [&](const ExprPtr& e) { return expr_tree_all(e, pred); })) return false;
    // (An int parameter turned into a float is guarded at the native entry, see entry_guard_params; native
    // callers passing a possibly-large int to such a parameter are refused in the call check above.)
    return conversions_ok(insts, fn, fn.decl->body, why);
}



// ============================================================
// Stage 1: eligibility scanner (syntax subset)
// ============================================================

class EligibilityScanner {
public:
    explicit EligibilityScanner(const std::unordered_map<std::string, FnInfo>& all_candidates)
        : candidates_(all_candidates) {}

    // Populates fn.calls_candidates and returns whether the syntax itself
    // (ignoring, for now, whether callees turn out eligible) is in the
    // numeric-only subset.
    bool scan(FnInfo& fn) {
        current_fn_ = &fn;
        calls_.clear();
        ok_ = true;
        why_.clear();
        active_for_.clear();
        for (const auto& s : fn.decl->body) scan_stmt(s, /*cond_ctx=*/false);
        fn.calls_candidates = calls_;
        return ok_;
    }

private:
    const std::unordered_map<std::string, FnInfo>& candidates_;
    const FnInfo* current_fn_ = nullptr;
    std::unordered_set<std::string> calls_;
    std::vector<std::string> active_for_;      // loop variables of the `for` loops currently being scanned
    bool ok_ = true;

    std::string why_;
public:
    const std::string& why() const { return why_; }
private:
    void fail(const char* why = "uses a construct the native subset does not support") {
        if (ok_) why_ = why;
        ok_ = false;
    }
    bool fn_array_param(const std::string& name) const {
        return current_fn_ && current_fn_->array_params.count(name) != 0;
    }

    void scan_stmt(const StmtPtr& s, bool cond_ctx) {
        if (!ok_ || !s) return;
        switch (s->kind) {
            case StmtKind::VarDecl:
                if (s->expr && s->expr->kind == ExprKind::Assign) {
                    if (!s->expr->target) { fail(); return; }
                    if (s->expr->target->kind == ExprKind::Index) scan_expr(s->expr->target, false);
                    else if (s->expr->target->kind != ExprKind::Ident ||
                             fn_array_param(s->expr->target->name)) {
                        fail("assigns to something other than a plain number variable or an f64_array element");
                        return;
                    }
                    scan_expr(s->expr->value, false);
                } else {
                    fail("declaration that is not a plain assignment");
                }
                break;
            case StmtKind::ExprStmt:
                if (s->expr && s->expr->kind == ExprKind::CompoundAssign) {
                    if (!s->expr->target ||
                        (s->expr->target->kind != ExprKind::Ident &&
                         s->expr->target->kind != ExprKind::Index)) { fail(); return; }
                    if (s->expr->op != TokenType::PLUS_EQ && s->expr->op != TokenType::MINUS_EQ) {
                        fail("uses a compound assignment other than += or -=");
                        return;
                    }
                    if (s->expr->target->kind == ExprKind::Index) {
                        scan_expr(s->expr->target, false);
                    } else if (fn_array_param(s->expr->target->name)) { fail(); return; }
                    scan_expr(s->expr->value, false);
                } else if (s->expr && s->expr->kind == ExprKind::Assign &&
                           s->expr->target && s->expr->target->kind == ExprKind::Index) {
                    scan_expr(s->expr->target, false);
                    scan_expr(s->expr->value, false);
                } else {
                    fail("has a bare call or output statement (e.g. bc::out, push): side effects are interpreted only");
                }
                break;
            case StmtKind::If:
                scan_expr(s->cond, true);
                for (const auto& bs : s->then_body) scan_stmt(bs, cond_ctx);
                for (const auto& el : s->elifs) {
                    scan_expr(el.first, true);
                    for (const auto& bs : el.second) scan_stmt(bs, cond_ctx);
                }
                for (const auto& bs : s->else_body) scan_stmt(bs, cond_ctx);
                break;
            case StmtKind::While:
                scan_expr(s->cond, true);
                for (const auto& bs : s->body) scan_stmt(bs, cond_ctx);
                break;
            case StmtKind::For: {
                // Only the `for i in thru N:` shape (desugared by the
                // parser into iterable == range(N)) - not general vector
                // iteration.
                auto& it = s->iterable;
                if (!it || it->kind != ExprKind::Call || !it->callee ||
                    it->callee->kind != ExprKind::Ident || it->callee->name != "range" ||
                    it->args.size() != 1) {
                    fail("has a 'for' loop over something other than 'thru N' (e.g. a vector or string)");
                    return;
                }
                scan_expr(it->args[0], false);
                // The interpreter keeps a `for` loop's hidden counters under the loop variable's NAME, so a
                // loop nested inside another loop with the same variable overwrites the outer loop's counters
                // and ends it early. Native code would run both loops in full, so such code stays interpreted.
                for (const auto& v : active_for_)
                    if (v == s->loop_var) {
                        const std::string msg =
                            "reuses the loop variable '" + user_visible_name(s->loop_var) +
                            "' in a loop nested inside another loop that already uses it; the interpreter shares "
                            "their hidden counters (the outer loop ends early), so native code would differ - "
                            "use a different name";
                        fail(msg.c_str());
                        return;
                    }
                active_for_.push_back(s->loop_var);
                for (const auto& bs : s->body) scan_stmt(bs, cond_ctx);
                active_for_.pop_back();
                break;
            }
            case StmtKind::Return:
                if (s->ret_value) scan_expr(s->ret_value, false);
                break;
            case StmtKind::Break:
            case StmtKind::Continue:
                break;
            default:
                fail("contains a nested function, try/catch, or other statement the native subset lacks");
                break;
        }
    }

    // `cond_ctx`: true if this expression's value is used directly as a
    // branch condition (an If/orf/While test, or a sub-expression of
    // && / || / ! that itself bottoms out in one) - the only place a
    // comparison or boolean value is allowed to appear. Everywhere else
    // (assigned, returned, passed as an argument) a bool would need a
    // third native type this pass doesn't carry, so it's simplest and
    // safest to just not allow it there.
    void scan_expr(const ExprPtr& e, bool cond_ctx) {
        if (!ok_ || !e) return;
        switch (e->kind) {
            case ExprKind::Int:
            case ExprKind::Float:
                break;
            case ExprKind::String:
                fail("uses a string");
                break;
            case ExprKind::Bool:
            case ExprKind::None:
                fail("uses a bool or none value");
                break;
            case ExprKind::VectorLiteral:
                fail("uses a vector literal");
                break;
            case ExprKind::DictLiteral:
                fail("uses a dictionary");
                break;
            case ExprKind::Member:
                fail("uses member access (bc::name or object.field)");
                break;
            case ExprKind::Index:
                if (!e->object || e->object->kind != ExprKind::Ident ||
                    !fn_array_param(e->object->name)) {
                    fail("indexes something that is not an f64_array parameter (vector, string or dict)");
                    return;
                }
                scan_expr(e->index, false);
                break;
            case ExprKind::Ident:
                if (fn_array_param(e->name)) fail("uses an f64_array as a plain value");
                break;
            case ExprKind::Unary:
                if (e->op == TokenType::NOT) {
                    if (!cond_ctx) { fail("uses a boolean (!) result as a number"); return; }
                    scan_expr(e->operand, true);
                } else if (e->op == TokenType::MINUS) {
                    scan_expr(e->operand, false);
                } else {
                    fail();
                }
                break;
            case ExprKind::Binary: {
                bool is_bool_op = (e->op == TokenType::LT || e->op == TokenType::GT ||
                                    e->op == TokenType::LE || e->op == TokenType::GE ||
                                    e->op == TokenType::EQ || e->op == TokenType::NEQ ||
                                    e->op == TokenType::AND || e->op == TokenType::OR);
                bool is_arith_op = (e->op == TokenType::PLUS || e->op == TokenType::MINUS ||
                                     e->op == TokenType::STAR || e->op == TokenType::SLASH ||
                                     e->op == TokenType::PERCENT);
                if (is_bool_op) {
                    if (!cond_ctx) { fail("stores or returns a comparison/boolean result (only allowed as an if/while condition)"); return; }
                    // && / || operands are themselves boolean context; a
                    // comparison's own operands (e.g. either side of `<`)
                    // are ordinary arithmetic values, not booleans.
                    bool operand_cond = (e->op == TokenType::AND || e->op == TokenType::OR);
                    scan_expr(e->left, operand_cond);
                    scan_expr(e->right, operand_cond);
                } else if (is_arith_op) {
                    scan_expr(e->left, false);
                    scan_expr(e->right, false);
                } else {
                    fail();
                }
                break;
            }
            case ExprKind::Call: {
                if (!e->callee || e->callee->kind != ExprKind::Ident) { fail(); return; }
                const std::string& name = e->callee->name;
                auto mit = math_whitelist().find(name);
                if (mit != math_whitelist().end()) {
                    size_t want = mit->second.kind == MathKind::AlwaysFloat1 ? 1
                                : mit->second.kind == MathKind::AlwaysFloat2 ? 2
                                : mit->second.kind == MathKind::AlwaysFloat3 ? 3
                                : 2; // Promote2
                    if (e->args.size() != want) { fail(); return; }
                } else if (candidates_.count(name)) {
                    calls_.insert(name);
                    const FnInfo& callee = candidates_.at(name);
                    if (e->args.size() != callee.param_names.size()) { fail(); return; }
                    for (size_t i = 0; i < e->args.size(); i++) {
                        if (callee.array_params.count(callee.param_names[i])) {
                            if (e->args[i]->kind != ExprKind::Ident ||
                                !fn_array_param(e->args[i]->name)) { fail(); return; }
                        } else {
                            scan_expr(e->args[i], false);
                        }
                    }
                    return;
                } else {
                    fail(("calls '" + name + "', which is neither a native-eligible function nor a whitelisted math builtin").c_str());
                    return;
                }
                for (const auto& a : e->args) scan_expr(a, false);
                break;
            }
            case ExprKind::Assign:
            case ExprKind::CompoundAssign:
                fail();  // only valid as a top-level statement, handled in scan_stmt
                break;
        }
    }
};

// A function that doesn't return on every control path implicitly
// returns `none` at the end, same as any other CSy function - but the
// native ABI only has int/float, nothing that can represent `none`.
// Silently returning 0 instead would be a real, dangerous divergence
// from the interpreted behavior (caught by testing `if x > 0: rt x*x;
// ::ext` called with a negative x - interpreted gives `none`, an
// earlier version of this file gave `0`), so a function must be proven
// to always hit an explicit `rt` before it's eligible at all. Loops are
// conservatively treated as "might not return" even if their body always
// does, since a `while`/`for` might execute zero times.
bool always_returns(const std::vector<StmtPtr>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::Return) return true;
        if (s->kind == StmtKind::If) {
            if (s->else_body.empty()) continue;  // no else -> might fall through
            if (!always_returns(s->then_body)) continue;
            bool all_elifs_return = std::all_of(s->elifs.begin(), s->elifs.end(),
                [](const std::pair<ExprPtr, std::vector<StmtPtr>>& el) { return always_returns(el.second); });
            if (all_elifs_return && always_returns(s->else_body)) return true;
        }
        // While/For/anything else: doesn't itself guarantee a return -
        // keep scanning the rest of this statement list.
    }
    return false;
}


// ============================================================
// Stage 2: type inference (per specialised instance)
// ============================================================

class TypeInferer {
public:
    TypeInferer(InstMap& insts, const InstMap& bases, std::vector<std::string>& order)
        : insts_(insts), bases_(bases), order_(order) {}

    bool overflowed() const { return overflow_; }

    // Create (or find) the instance of `base` for argument signature `sig`.
    // Returns its key, or "" if the instance cap has been reached.
    std::string ensure(const std::string& base, const std::string& sig) {
        const std::string key = base + "$" + sig;
        if (insts_.count(key)) return key;
        if (insts_.size() >= kMaxInstances) { overflow_ = true; return ""; }
        FnInfo fn = bases_.at(base);
        fn.bases = &bases_;
        fn.base_name = base;
        fn.sig = sig;
        fn.key = key;
        for (size_t i = 0; i < fn.param_names.size() && i < sig.size(); i++) {
            const std::string& p = fn.param_names[i];
            switch (sig[i]) {
                case 'A': fn.local_types[p] = NType::Array; break;
                case 'I': fn.local_types[p] = NType::Int;   fn.seen[p] = 1; break;
                case 'F': fn.local_types[p] = NType::Float; fn.seen[p] = 2; break;
                default:  fn.local_types[p] = NType::Float; fn.seen[p] = 3; break;  // 'M'
            }
        }
        collect_locals(fn.decl->body, fn.local_types);
        insts_.emplace(key, std::move(fn));
        order_.push_back(key);
        changed_ = true;
        return key;
    }

    void run() {
        // Types only ever move up the lattice (Int -> Float; "seen" bits only get
        // set), and the number of instances is capped, so this terminates.
        for (int iter = 0; iter < 10000; iter++) {
            changed_ = false;
            for (size_t i = 0; i < order_.size(); i++) {  // order_ can grow while we iterate
                FnInfo& fn = insts_.at(order_[i]);
                for (const auto& s : fn.decl->body) propagate_stmt(fn, s);
            }
            if (!changed_) break;
        }
    }

private:
    InstMap& insts_;
    const InstMap& bases_;
    std::vector<std::string>& order_;
    bool changed_ = false;
    bool overflow_ = false;

    void collect_locals(const std::vector<StmtPtr>& body, std::unordered_map<std::string, NType>& out) {
        for (const auto& s : body) {
            if (!s) continue;
            switch (s->kind) {
                case StmtKind::VarDecl:
                    if (s->expr && s->expr->kind == ExprKind::Assign && s->expr->target &&
                        s->expr->target->kind == ExprKind::Ident) {
                        out.emplace(s->expr->target->name, NType::Int);
                    }
                    break;
                case StmtKind::If:
                    collect_locals(s->then_body, out);
                    for (const auto& el : s->elifs) collect_locals(el.second, out);
                    collect_locals(s->else_body, out);
                    break;
                case StmtKind::While:
                    collect_locals(s->body, out);
                    break;
                case StmtKind::For:
                    out.emplace(s->loop_var, NType::Int);
                    collect_locals(s->body, out);
                    break;
                default:
                    break;
            }
        }
    }

    NType type_of(FnInfo& fn, const ExprPtr& e) { return infer_expr_type(insts_, fn, e); }

    // Record that `name` is assigned a value of type `t`; `certain_float` says
    // the value is a float on every path (see definitely_float).
    void assign(FnInfo& fn, const std::string& name, NType t, bool certain_float) {
        auto it = fn.local_types.find(name);
        if (it == fn.local_types.end()) { fn.local_types[name] = t; changed_ = true; it = fn.local_types.find(name); }
        else if (t == NType::Float && it->second == NType::Int) { it->second = NType::Float; changed_ = true; }
        const int bits = t == NType::Int ? 1 : (certain_float ? 2 : 3);
        int& seen = fn.seen[name];
        if ((seen | bits) != seen) { seen |= bits; changed_ = true; }
    }

    // Walk an expression only to make sure every user-function call has an
    // instance for the argument types it is made with.
    void discover(FnInfo& fn, const ExprPtr& e) {
        expr_tree_all(e, [&](const ExprPtr& n) {
            if (n->kind == ExprKind::Call && n->callee && n->callee->kind == ExprKind::Ident &&
                math_whitelist().count(n->callee->name) == 0) {
                std::string key, base, sig;
                if (callee_key(insts_, fn, n, key, &base, &sig)) ensure(base, sig);
            }
            return true;
        });
    }

    void propagate_stmt(FnInfo& fn, const StmtPtr& s) {
        if (!s) return;
        discover(fn, s->expr); discover(fn, s->cond); discover(fn, s->ret_value);
        discover(fn, s->kind == StmtKind::For && s->iterable && !s->iterable->args.empty()
                         ? s->iterable->args[0] : s->iterable);
        for (const auto& el : s->elifs) discover(fn, el.first);
        switch (s->kind) {
            case StmtKind::VarDecl:
                if (s->expr && s->expr->kind == ExprKind::Assign && s->expr->target &&
                    s->expr->target->kind == ExprKind::Ident) {
                    const NType t = type_of(fn, s->expr->value);
                    assign(fn, s->expr->target->name, t, definitely_float(insts_, fn, s->expr->value));
                }
                break;
            case StmtKind::ExprStmt:
                if (s->expr && s->expr->kind == ExprKind::CompoundAssign && s->expr->target &&
                    s->expr->target->kind == ExprKind::Ident) {
                    // x += v  stores x + v (or x - v)
                    const std::string& name = s->expr->target->name;
                    const NType tx = fn.local_types.count(name) ? fn.local_types[name] : NType::Int;
                    const NType tv = type_of(fn, s->expr->value);
                    const NType t = (tx == NType::Float || tv == NType::Float) ? NType::Float : NType::Int;
                    const bool x_certain = tx == NType::Float && (fn.seen[name] & 1) == 0;
                    assign(fn, name, t, x_certain || definitely_float(insts_, fn, s->expr->value));
                }
                break;
            case StmtKind::If:
                for (const auto& bs : s->then_body) propagate_stmt(fn, bs);
                for (const auto& el : s->elifs)
                    for (const auto& bs : el.second) propagate_stmt(fn, bs);
                for (const auto& bs : s->else_body) propagate_stmt(fn, bs);
                break;
            case StmtKind::While:
                for (const auto& bs : s->body) propagate_stmt(fn, bs);
                break;
            case StmtKind::For:
                assign(fn, s->loop_var, NType::Int, false);  // the loop variable always receives ints
                for (const auto& bs : s->body) propagate_stmt(fn, bs);
                break;
            case StmtKind::Return:
                if (s->ret_value) {
                    fn.has_return = true;
                    const NType t = type_of(fn, s->ret_value);
                    if (t == NType::Float && fn.return_type == NType::Int) { fn.return_type = NType::Float; changed_ = true; }
                    const int bits = t == NType::Int ? 1 : (definitely_float(insts_, fn, s->ret_value) ? 2 : 3);
                    if ((fn.ret_seen | bits) != fn.ret_seen) { fn.ret_seen |= bits; changed_ = true; }
                }
                break;
            default:
                break;
        }
    }
};

// A bare `rt;` returns none in the interpreter; native code has no none.
bool has_bare_return(const std::vector<StmtPtr>& body) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::Return && !s->ret_value) return true;
        if (has_bare_return(s->body) || has_bare_return(s->then_body) || has_bare_return(s->else_body)) return true;
        for (const auto& el : s->elifs) if (has_bare_return(el.second)) return true;
    }
    return false;
}

// ============================================================
// Stage 3: C codegen (one C function per instance)
// ============================================================

class CCodeGen {
public:
    explicit CCodeGen(InstMap& insts) : fns_(insts) {}

    // `emit_order`: keys of the instances to emit (ids already assigned).
    // `exported`: the subset the VM may call directly.
    std::string generate(const std::vector<std::string>& emit_order,
                         const std::vector<std::string>& exported) {
        std::ostringstream out;
        out << kPreamble << "\n";
        for (const auto& key : emit_order) out << signature(fns_.at(key)) << ";\n";
        out << "\n";
        for (const auto& key : emit_order) emit_function(out, fns_.at(key));
        for (const auto& key : exported) emit_entry(out, fns_.at(key));
        out << "typedef struct { const char* name; const char* sig; "
               "void (*entry)(const CsyNativeArg*, int*, int*, CsyRet*); int ret_float; } CsyEntryRow;\n";
        out << "typedef struct { const char* name; const char* sig; int user_nparams; "
               "const char* const* global_names; int global_count; } CsyGlobalRow;\n";
        for (const auto& key : exported) {
            const FnInfo& f = fns_.at(key);
            if (f.global_params.empty()) continue;
            out << "static const char* csy_gnames" << f.id << "[] = {";
            for (size_t i = 0; i < f.global_params.size(); i++) {
                if (i) out << ", ";
                out << "\"" << c_escape(f.global_params[i]) << "\"";
            }
            out << "};\n";
        }
        out << "const CsyEntryRow csy_entry_table[] = {\n";
        for (const auto& key : exported) {
            const FnInfo& f = fns_.at(key);
            out << "  {\"" << c_escape(f.base_name) << "\", \"" << f.sig << "\", csy_entry" << f.id << ", "
                << (f.return_type == NType::Float ? 1 : 0) << "},\n";
        }
        out << "  {0, 0, 0, 0}\n};\n";
        out << "const int csy_entry_count = " << exported.size() << ";\n";
        out << "const CsyGlobalRow csy_global_table[] = {\n";
        for (const auto& key : exported) {
            const FnInfo& f = fns_.at(key);
            out << "  {\"" << c_escape(f.base_name) << "\", \"" << f.sig << "\", " << f.user_param_count << ", "
                << (f.global_params.empty() ? "0" : "csy_gnames" + std::to_string(f.id)) << ", "
                << f.global_params.size() << "},\n";
        }
        out << "  {0, 0, 0, 0, 0}\n};\n";
        out << "const int csy_global_count = " << exported.size() << ";\n";
        return out.str();
    }

private:
    InstMap& fns_;
    size_t temp_id_ = 0;
    std::unordered_map<const Expr*, size_t> call_tag_ids_;

    bool has_dynamic_tag(const FnInfo& fn, const std::string& name) const {
        auto it = fn.seen.find(name);
        return it != fn.seen.end() && (it->second & 3) == 3;
    }
    std::string tag_name(const std::string& name) const { return "tag_" + vname(name); }
    std::string call_tag_name(const ExprPtr& e) {
        auto it = call_tag_ids_.find(e.get());
        if (it == call_tag_ids_.end()) it = call_tag_ids_.emplace(e.get(), temp_id_++).first;
        return "__csy_call_tag_" + std::to_string(it->second);
    }

    bool has_dynamic_call_tag(FnInfo& fn, const ExprPtr& e) {
        bool found = false;
        expr_tree_all(e, [&](const ExprPtr& node) {
            if (node && node->kind == ExprKind::Call && node->callee && node->callee->kind == ExprKind::Ident &&
                !math_whitelist().count(node->callee->name)) {
                std::string key;
                if (callee_key(fns_, fn, node, key) && returns_maybe_int(fns_.at(key))) found = true;
            }
            return !found;
        });
        return found;
    }

    static std::string c_escape(const std::string& s) {
        std::string r;
        for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
        return r;
    }
    static std::string c_type(NType t) { return t == NType::Float ? "double" : "long long"; }
    static std::string vname(const std::string& n) { return "v_" + n; }
    static std::string fname(const FnInfo& f) { return "csy_fn" + std::to_string(f.id); }

    // Typed C signature: int params are `long long`, float params `double`,
    // array params a pointer + length. Nothing is squeezed through a double.
    std::string signature(const FnInfo& fn) {
        std::ostringstream s;
        s << "static " << c_type(fn.return_type) << " " << fname(fn) << "(";
        for (size_t i = 0; i < fn.param_names.size(); i++) {
            const std::string& p = fn.param_names[i];
            switch (fn.sig[i]) {
                case 'A': s << "double* p_" << p << ", long long plen_" << p << ", "; break;
                case 'I': s << "long long p_" << p << ", "; break;
                case 'M': s << "double p_" << p << ", int p_tag_" << p << ", "; break;
                default:  s << "double p_" << p << ", "; break;
            }
        }
        s << "int __depth, int* __err, int* __rtag)";
        return s.str();
    }

    // Exported wrapper with the uniform ABI the VM calls through.
    void emit_entry(std::ostringstream& out, const FnInfo& fn) {
        out << "static void csy_entry" << fn.id << "(const CsyNativeArg* a, int* err, int* rtag, CsyRet* out) {\n";
        for (size_t gi : entry_guard_params(fn))
            out << "  if (a[" << gi << "].ival >= 9007199254740992LL || a[" << gi << "].ival <= -9007199254740992LL) { *err = 5; return; }\n";
        out << "  " << c_type(fn.return_type) << " r = " << fname(fn) << "(";
        for (size_t i = 0; i < fn.param_names.size(); i++) {
            switch (fn.sig[i]) {
                case 'A': out << "a[" << i << "].data, a[" << i << "].length, "; break;
                case 'I': out << "a[" << i << "].ival, "; break;
                default:  out << "a[" << i << "].number, "; break;
            }
        }
        out << "0, err, rtag);\n";
        out << (fn.return_type == NType::Float ? "  out->f = r;\n" : "  out->i = r;\n");
        out << "}\n\n";
    }

    void emit_function(std::ostringstream& out, FnInfo& fn) {
        call_tag_ids_.clear();
        out << signature(fn) << " {\n";
        out << "  if (__depth > CSY_MAX_NATIVE_DEPTH) { *__err = 2; return 0; }\n";
        out << "  int __dummy_tag = 0; (void)__dummy_tag; (void)__rtag;\n";
        all_stmt_exprs(fn.decl->body, [&](const ExprPtr& e) {
            expr_tree_all(e, [&](const ExprPtr& node) {
                if (node && node->kind == ExprKind::Call && node->callee && node->callee->kind == ExprKind::Ident &&
                    !math_whitelist().count(node->callee->name)) {
                    std::string key;
                    if (callee_key(fns_, fn, node, key) && returns_maybe_int(fns_.at(key))) call_tag_name(node);
                }
                return true;
            });
            return true;
        });
        for (const auto& [node, id] : call_tag_ids_) {
            (void)node;
            out << "  int __csy_call_tag_" << id << " = 0;\n";
        }
        // Params become typed locals (a param assigned a float in the body is
        // promoted, so the local may be wider than the parameter); every other
        // local is declared up front because CSy is function-scoped.
        for (size_t i = 0; i < fn.param_names.size(); i++) {
            const std::string& p = fn.param_names[i];
            const NType t = fn.local_types.at(p);
            if (t == NType::Array) {
                out << "  double* " << vname(p) << " = p_" << p << ";\n";
                out << "  long long len_" << p << " = plen_" << p << ";\n";
            } else {
                out << "  " << c_type(t) << " " << vname(p) << " = (" << c_type(t) << ")p_" << p << ";\n";
                if (has_dynamic_tag(fn, p)) {
                    out << "  int " << tag_name(p) << " = "
                        << (fn.sig[i] == 'M' ? "p_tag_" + p : (fn.sig[i] == 'F' ? "1" : "0")) << ";\n";
                }
            }
        }
        for (const auto& [local, t] : fn.local_types) {
            if (std::find(fn.param_names.begin(), fn.param_names.end(), local) != fn.param_names.end()) continue;
            out << "  " << c_type(t) << " " << vname(local) << " = 0;\n";
            if (has_dynamic_tag(fn, local)) out << "  int " << tag_name(local) << " = 0;\n";
        }
        for (const auto& s : fn.decl->body) emit_stmt(out, fn, s, 1);
        out << "  return 0;\n";
        out << "}\n\n";
    }

    void indent(std::ostringstream& out, int n) { for (int i = 0; i < n; i++) out << "  "; }

    // "is this value truthy" exactly as the interpreter decides it (non-zero),
    // for ints and floats alike - a plain (long long) cast would call 0.5 false.
    std::string truthy(FnInfo& fn, const ExprPtr& e) { return "((" + expr(fn, e) + ") != 0)"; }

    void emit_stmt(std::ostringstream& out, FnInfo& fn, const StmtPtr& s, int depth) {
        if (!s) return;
        switch (s->kind) {
            case StmtKind::VarDecl:
                indent(out, depth);
                if (s->expr->target->kind == ExprKind::Index) {
                    const auto& target = s->expr->target;
                    out << "csy_array_set(" << vname(target->object->name) << ", len_" << target->object->name
                        << ", (long long)(" << expr(fn, target->index) << "), "
                        << expr(fn, s->expr->value) << ", __err);\n";
                } else {
                    const std::string& target = s->expr->target->name;
                    out << vname(target) << " = " << expr(fn, s->expr->value) << ";\n";
                    if (has_dynamic_tag(fn, target))
                        out << "  " << tag_name(target) << " = " << tag_expr(fn, s->expr->value) << ";\n";
                }
                indent(out, depth); out << "if (*__err) return 0;\n";
                break;
            case StmtKind::ExprStmt: {
                indent(out, depth);
                if (s->expr->kind == ExprKind::Assign) {
                    const auto& target = s->expr->target;
                    out << "csy_array_set(" << vname(target->object->name) << ", len_" << target->object->name
                        << ", (long long)(" << expr(fn, target->index) << "), "
                        << expr(fn, s->expr->value) << ", __err);\n";
                } else if (s->expr->target->kind == ExprKind::Index) {
                    const auto& target = s->expr->target;
                    out << "csy_array_compound(" << vname(target->object->name) << ", len_"
                        << target->object->name << ", (long long)(" << expr(fn, target->index)
                        << "), " << expr(fn, s->expr->value) << ", "
                        << (s->expr->op == TokenType::MINUS_EQ ? "1" : "0") << ", __err);\n";
                } else {
                    const std::string target = vname(s->expr->target->name);
                    const TokenType op = s->expr->op == TokenType::PLUS_EQ ? TokenType::PLUS : TokenType::MINUS;
                    if (infer_expr_type(fns_, fn, s->expr->target) == NType::Int) {
                        out << target << " = " << safe_integer_arithmetic(op, target, expr(fn, s->expr->value)) << ";\n";
                    } else {
                        out << target << (op == TokenType::PLUS ? " += " : " -= ")
                            << expr(fn, s->expr->value) << ";\n";
                    }
                    if (has_dynamic_tag(fn, s->expr->target->name)) {
                        auto result = std::make_shared<Expr>();
                        result->kind = ExprKind::Binary;
                        result->op = op;
                        result->left = s->expr->target;
                        result->right = s->expr->value;
                        out << "  " << tag_name(s->expr->target->name) << " = " << tag_expr(fn, result) << ";\n";
                    }
                }
                indent(out, depth); out << "if (*__err) return 0;\n";
                break;
            }
            case StmtKind::If: {
                const std::string cond = "__csy_cond_" + std::to_string(temp_id_++);
                indent(out, depth);
                out << "{ int " << cond << " = " << truthy(fn, s->cond) << ";\n";
                indent(out, depth + 1); out << "if (*__err) return 0;\n";
                indent(out, depth + 1); out << "if (" << cond << ") {\n";
                for (const auto& bs : s->then_body) emit_stmt(out, fn, bs, depth + 1);
                indent(out, depth + 1); out << "}";
                for (const auto& el : s->elifs) {
                    const std::string elif_cond = "__csy_cond_" + std::to_string(temp_id_++);
                    out << " else { int " << elif_cond << " = " << truthy(fn, el.first) << ";\n";
                    indent(out, depth + 2); out << "if (*__err) return 0;\n";
                    indent(out, depth + 2); out << "if (" << elif_cond << ") {\n";
                    for (const auto& bs : el.second) emit_stmt(out, fn, bs, depth + 1);
                    indent(out, depth + 2); out << "}";
                }
                if (!s->else_body.empty()) {
                    out << " else {\n";
                    for (const auto& bs : s->else_body) emit_stmt(out, fn, bs, depth + 1);
                    indent(out, depth + 2); out << "}";
                }
                for (size_t i = 0; i < s->elifs.size(); i++) out << " }";
                out << " }\n";
                break;
            }
            case StmtKind::While: {
                const size_t id = temp_id_++;
                indent(out, depth); out << "while (1) {\n";
                indent(out, depth + 1);
                out << "int __csy_cond_" << id << " = " << truthy(fn, s->cond) << ";\n";
                indent(out, depth + 1); out << "if (*__err) return 0;\n";
                indent(out, depth + 1); out << "if (!__csy_cond_" << id << ") break;\n";
                for (const auto& bs : s->body) emit_stmt(out, fn, bs, depth + 1);
                indent(out, depth); out << "}\n";
                break;
            }
            case StmtKind::For: {
                // The interpreter counts with a hidden index and *assigns* the loop
                // variable from it each round: the body may overwrite the variable
                // without affecting iteration, and afterwards it holds the last
                // value assigned. A separate counter reproduces both.
                const std::string lv = vname(s->loop_var);
                const size_t id = temp_id_++;
                const std::string bound = "__csy_bound_" + std::to_string(id);
                const std::string ctr = "__csy_ctr_" + std::to_string(id);
                const NType bt = infer_expr_type(fns_, fn, s->iterable->args[0]);
                indent(out, depth);
                out << "{ long long " << bound << " = "
                    << (bt == NType::Float ? "csy_bound_f(" + expr(fn, s->iterable->args[0]) + ")"
                                           : "(" + expr(fn, s->iterable->args[0]) + ")") << ";\n";
                indent(out, depth + 1); out << "if (*__err) return 0;\n";
                indent(out, depth + 1);
                out << "for (long long " << ctr << " = 0; " << ctr << " < " << bound << "; " << ctr << "++) {\n";
                indent(out, depth + 2); out << lv << " = " << ctr << ";\n";
                for (const auto& bs : s->body) emit_stmt(out, fn, bs, depth + 2);
                indent(out, depth + 1); out << "}\n";
                indent(out, depth); out << "}\n";
                break;
            }
            case StmtKind::Return: {
                // Evaluate first, then tag: nested calls inside the expression write
                // to a scratch tag, never to ours.
                const NType t = infer_expr_type(fns_, fn, s->ret_value);
                indent(out, depth);
                out << "{ " << c_type(fn.return_type) << " __r = (" << c_type(fn.return_type) << ")("
                    << expr(fn, s->ret_value) << "); if (*__err) return 0; *__rtag = ";
                if (returns_maybe_int(fn)) out << tag_expr(fn, s->ret_value);
                else out << (t == NType::Float ? 1 : 0);
                out << "; return __r; }\n";
                break;
            }
            case StmtKind::Break:
                indent(out, depth); out << "break;\n";
                break;
            case StmtKind::Continue:
                indent(out, depth); out << "continue;\n";
                break;
            default:
                break;
        }
    }

    std::string safe_div(FnInfo& fn, const ExprPtr& e) {
        const bool is_float = infer_expr_type(fns_, fn, e) == NType::Float;
        std::ostringstream s;
        s << (is_float ? "csy_safe_div_f(" : "csy_safe_div_i(")
          << expr(fn, e->left) << ", " << expr(fn, e->right) << ", __err)";
        return s.str();
    }

    std::string safe_integer_arithmetic(TokenType op, const std::string& left, const std::string& right) {
        const char* helper = op == TokenType::PLUS ? "csy_safe_add_i" :
                             op == TokenType::MINUS ? "csy_safe_sub_i" :
                             op == TokenType::STAR ? "csy_safe_mul_i" : "csy_safe_mod_i";
        return std::string(helper) + "(" + left + ", " + right + ", __err)";
    }

    // Shortest decimal text that reads back as exactly the same double - the
    // default ostream precision (6 digits) silently changed every literal.
    static std::string float_literal(double v) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        std::string s = buf;
        if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";  // keep it a C double
        return s;
    }

    std::string expr(FnInfo& fn, const ExprPtr& e) {
        switch (e->kind) {
            case ExprKind::Int: return std::to_string(e->int_val) + "LL";
            case ExprKind::Float: return float_literal(e->float_val);
            case ExprKind::Ident: return vname(e->name);
            case ExprKind::Unary:
                if (e->op == TokenType::MINUS) {
                    const std::string operand = expr(fn, e->operand);
                    if (infer_expr_type(fns_, fn, e->operand) == NType::Int)
                        return "csy_safe_neg_i(" + operand + ", __err)";
                    return "(-(" + operand + "))";
                }
                return "(!(" + expr(fn, e->operand) + "))";
            case ExprKind::Binary: {
                if (e->op == TokenType::SLASH) return "(" + safe_div(fn, e) + ")";
                if ((e->op == TokenType::PLUS || e->op == TokenType::MINUS ||
                     e->op == TokenType::STAR || e->op == TokenType::PERCENT) &&
                    infer_expr_type(fns_, fn, e) == NType::Int) {
                    return safe_integer_arithmetic(e->op, expr(fn, e->left), expr(fn, e->right));
                }
                const char* op =
                    e->op == TokenType::PLUS ? "+" : e->op == TokenType::MINUS ? "-" :
                    e->op == TokenType::STAR ? "*" : e->op == TokenType::PERCENT ? "%" :
                    e->op == TokenType::LT ? "<" : e->op == TokenType::GT ? ">" :
                    e->op == TokenType::LE ? "<=" : e->op == TokenType::GE ? ">=" :
                    e->op == TokenType::EQ ? "==" : e->op == TokenType::NEQ ? "!=" :
                    e->op == TokenType::AND ? "&&" : "||";
                // `%` between doubles is not valid C - deliberately: validation
                // guarantees both sides are ints here, and if that were ever
                // violated the C compile would fail loudly (whole-program
                // fallback to the interpreter) instead of computing a wrong value.
                return "(" + expr(fn, e->left) + " " + op + " " + expr(fn, e->right) + ")";
            }
            case ExprKind::Call: {
                const std::string& name = e->callee->name;
                auto mit = math_whitelist().find(name);
                if (mit != math_whitelist().end()) {
                    if (mit->second.kind == MathKind::Promote2) {
                        NType a = infer_expr_type(fns_, fn, e->args[0]), b = infer_expr_type(fns_, fn, e->args[1]);
                        bool is_float = (a == NType::Float || b == NType::Float);
                        std::string f = (name == "max") ? (is_float ? "fmax" : "csy_max_i")
                                                        : (is_float ? "fmin" : "csy_min_i");
                        return f + "(" + expr(fn, e->args[0]) + ", " + expr(fn, e->args[1]) + ")";
                    }
                    std::string t = mit->second.c_expr_template;
                    for (size_t i = 0; i < e->args.size(); i++) {
                        std::string ph = "{" + std::to_string(i) + "}";
                        size_t pos = t.find(ph);
                        if (pos != std::string::npos) t.replace(pos, ph.size(), "(double)(" + expr(fn, e->args[i]) + ")");
                    }
                    return "(" + t + ")";
                }
                // Call into the instance specialised for exactly these argument types.
                std::string key;
                callee_key(fns_, fn, e, key);
                const FnInfo& callee = fns_.at(key);
                bool sequence_args = false;
                for (size_t i = 0; i < e->args.size(); i++)
                    if (callee.sig[i] == 'M' && has_dynamic_call_tag(fn, e->args[i])) sequence_args = true;
                if (sequence_args) {
                    const size_t id = temp_id_++;
                    std::ostringstream staged, call;
                    call << fname(callee) << "(";
                    for (size_t i = 0; i < e->args.size(); i++) {
                        const ExprPtr& arg = e->args[i];
                        const std::string v = "__csy_arg_v_" + std::to_string(id) + "_" + std::to_string(i);
                        const std::string t = "__csy_arg_t_" + std::to_string(id) + "_" + std::to_string(i);
                        switch (callee.sig[i]) {
                            case 'A':
                                staged << " double* " << v << " = " << vname(arg->name) << ";"
                                       << " long long __csy_arg_n_" << id << "_" << i << " = len_" << arg->name << ";";
                                call << v << ", __csy_arg_n_" << id << "_" << i << ", ";
                                break;
                            case 'I':
                                staged << " long long " << v << " = (long long)(" << expr(fn, arg) << ");";
                                call << v << ", ";
                                break;
                            case 'M':
                                staged << " double " << v << " = (double)(" << expr(fn, arg) << ");"
                                       << " int " << t << " = " << tag_expr(fn, arg) << ";";
                                call << v << ", " << t << ", ";
                                break;
                            default:
                                staged << " double " << v << " = (double)(" << expr(fn, arg) << ");";
                                call << v << ", ";
                                break;
                        }
                    }
                    for (size_t i = 0; i < callee.global_params.size(); i++) {
                        const size_t p = callee.user_param_count + i;
                        const std::string value = vname(callee.global_params[i]);
                        if (callee.sig[p] == 'I') call << "(long long)(" << value << "), ";
                        else call << "(double)(" << value << "), ";
                    }
                    const std::string tag_out = returns_maybe_int(callee) ? "&" + call_tag_name(e) : "&__dummy_tag";
                    call << "__depth + 1, __err, " << tag_out << ")";
                    return "({" + staged.str() + " " + call.str() + "; })";
                }
                std::ostringstream args;
                for (size_t i = 0; i < e->args.size(); i++) {
                    switch (callee.sig[i]) {
                        case 'A': args << vname(e->args[i]->name) << ", len_" << e->args[i]->name << ", "; break;
                        case 'I': args << "(long long)(" << expr(fn, e->args[i]) << "), "; break;
                        case 'M': args << "(double)(" << expr(fn, e->args[i]) << "), " << tag_expr(fn, e->args[i]) << ", "; break;
                        default:  args << "(double)(" << expr(fn, e->args[i]) << "), "; break;
                    }
                }
                for (size_t i = 0; i < callee.global_params.size(); i++) {
                    const size_t p = callee.user_param_count + i;
                    const std::string value = vname(callee.global_params[i]);
                    if (callee.sig[p] == 'I') args << "(long long)(" << value << "), ";
                    else args << "(double)(" << value << "), ";
                }
                const std::string tag_out = returns_maybe_int(callee) ? "&" + call_tag_name(e) : "&__dummy_tag";
                return fname(callee) + "(" + args.str() + "__depth + 1, __err, " + tag_out + ")";
            }
            case ExprKind::Index: {
                const std::string& name = e->object->name;
                return "csy_array_get(" + vname(name) + ", len_" + name + ", (long long)(" +
                       expr(fn, e->index) + "), __err)";
            }
            default:
                return "0";
        }
    }

    std::string tag_expr(FnInfo& fn, const ExprPtr& e) {
        if (!e) return "0";
        switch (e->kind) {
            case ExprKind::Int: return "0";
            case ExprKind::Float: return "1";
            case ExprKind::Ident: {
                auto t = fn.local_types.find(e->name);
                if (t == fn.local_types.end() || t->second != NType::Float) return "0";
                return has_dynamic_tag(fn, e->name) ? tag_name(e->name) : "1";
            }
            case ExprKind::Unary:
                return e->op == TokenType::MINUS ? tag_expr(fn, e->operand) : "0";
            case ExprKind::Binary:
                if (e->op == TokenType::PLUS || e->op == TokenType::MINUS || e->op == TokenType::STAR ||
                    e->op == TokenType::SLASH)
                    return "(" + tag_expr(fn, e->left) + " | " + tag_expr(fn, e->right) + ")";
                return "0";
            case ExprKind::Index: return "1";
            case ExprKind::Call: {
                const std::string& name = e->callee->name;
                auto mit = math_whitelist().find(name);
                if (mit != math_whitelist().end()) {
                    if (mit->second.kind != MathKind::Promote2) return "1";
                    return "(" + tag_expr(fn, e->args[0]) + " | " + tag_expr(fn, e->args[1]) + ")";
                }
                std::string key;
                if (!callee_key(fns_, fn, e, key)) return "0";
                const FnInfo& callee = fns_.at(key);
                return returns_maybe_int(callee) ? call_tag_name(e) : (callee.return_type == NType::Float ? "1" : "0");
            }
            default: return "0";
        }
    }
};

#include "x64_backend.inc"

namespace {

void find_array_params_expr(const ExprPtr& e, const std::unordered_set<std::string>& params,
                            std::unordered_set<std::string>& arrays) {
    if (!e) return;
    if (e->kind == ExprKind::Index && e->object && e->object->kind == ExprKind::Ident &&
        params.count(e->object->name)) {
        arrays.insert(e->object->name);
    }
    find_array_params_expr(e->left, params, arrays);
    find_array_params_expr(e->right, params, arrays);
    find_array_params_expr(e->operand, params, arrays);
    find_array_params_expr(e->target, params, arrays);
    find_array_params_expr(e->value, params, arrays);
    find_array_params_expr(e->callee, params, arrays);
    find_array_params_expr(e->object, params, arrays);
    find_array_params_expr(e->index, params, arrays);
    for (const auto& arg : e->args) find_array_params_expr(arg, params, arrays);
    for (const auto& item : e->elements) find_array_params_expr(item, params, arrays);
    for (const auto& pair : e->dict_pairs) {
        find_array_params_expr(pair.first, params, arrays);
        find_array_params_expr(pair.second, params, arrays);
    }
}

void find_array_params_stmts(const std::vector<StmtPtr>& body,
                             const std::unordered_set<std::string>& params,
                             std::unordered_set<std::string>& arrays) {
    for (const auto& s : body) {
        if (!s) continue;
        find_array_params_expr(s->expr, params, arrays);
        find_array_params_expr(s->cond, params, arrays);
        find_array_params_expr(s->iterable, params, arrays);
        find_array_params_expr(s->ret_value, params, arrays);
        find_array_params_stmts(s->body, params, arrays);
        find_array_params_stmts(s->then_body, params, arrays);
        find_array_params_stmts(s->else_body, params, arrays);
        for (const auto& branch : s->elifs) {
            find_array_params_expr(branch.first, params, arrays);
            find_array_params_stmts(branch.second, params, arrays);
        }
    }
}

} // namespace

// ============================================================
// Stage 4: compile + load
// ============================================================

namespace {

// Two independent 64-bit FNV-1a passes (128 bits total): the cache key must
// make an accidental collision between two different generated sources
// practically impossible, and std::hash (64-bit, unspecified) did not.
std::string hash_hex(const std::string& s) { return hash128_hex(s); }

bool path_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

std::string cache_dir() {
    // CSY_JIT_CACHE_DIR relocates the cache (read-only /tmp, containers, tests). The same
    // ownership and permission checks apply wherever it is.
    const char* override_dir = std::getenv("CSY_JIT_CACHE_DIR");
    const std::string dir = (override_dir && *override_dir) ? std::string(override_dir)
                                                            : "/tmp/csy_native_jit-" + std::to_string(::geteuid());
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return {};
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) return {};
    if ((st.st_mode & 0077) != 0 && ::chmod(dir.c_str(), 0700) != 0) return {};
    return dir;
}

// The cache is content-addressed and otherwise grows forever. Drop entries
// untouched for two weeks, and the oldest ones beyond a few hundred files.
void prune_cache(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct Entry { std::string path; time_t mtime; };
    std::vector<Entry> entries;
    while (struct dirent* de = ::readdir(d)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        const std::string path = dir + "/" + name;
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) entries.push_back({path, st.st_mtime});
    }
    ::closedir(d);
    const time_t now = ::time(nullptr);
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.mtime > y.mtime; });
    for (size_t i = 0; i < entries.size(); i++) {
        if (now - entries[i].mtime > 14 * 24 * 3600 || i >= 300) ::unlink(entries[i].path.c_str());
    }
}

// Write via a temp file and rename, so a crash or concurrent writer never leaves
// a half-written file under the final (content-addressed) name.
bool write_file_atomic(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp" + std::to_string(::getpid());
    res::register_temp_path(tmp);
    const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool ok = fd >= 0;
    if (ok) {
        size_t written = 0;
        while (written < content.size()) {
            const ssize_t count = ::write(fd, content.data() + written, content.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { ok = false; break; }
            written += static_cast<size_t>(count);
        }
        ok = ok && ::fsync(fd) == 0;
        ok = ::close(fd) == 0 && ok;
    }
    ok = ok && ::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) ::unlink(tmp.c_str());
    res::unregister_temp_path(tmp);
    return ok;
}

bool split_command_words(const std::string& command, std::vector<std::string>& words) {
    std::string word;
    char quote = 0;
    bool escaping = false;
    bool active = false;
    for (char c : command) {
        if (escaping) { word += c; escaping = false; active = true; continue; }
        if (c == '\\' && quote != '\'') { escaping = true; active = true; continue; }
        if (quote) {
            if (c == quote) quote = 0;
            else word += c;
            active = true;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; active = true; continue; }
        if (c == ' ' || c == '\t' || c == '\n') {
            if (active) { words.push_back(std::move(word)); word.clear(); active = false; }
            continue;
        }
        word += c;
        active = true;
    }
    if (escaping || quote) return false;
    if (active) words.push_back(std::move(word));
    return !words.empty();
}

int run_compiler(const std::vector<std::string>& command, const std::string& error_path) {
    const pid_t child = ::fork();
    if (child < 0) return -1;
    if (child == 0) {
        const int err_fd = ::open(error_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (err_fd < 0 || ::dup2(err_fd, STDERR_FILENO) < 0) _exit(126);
        if (err_fd != STDERR_FILENO) ::close(err_fd);
        std::vector<char*> args;
        args.reserve(command.size() + 1);
        for (const auto& part : command) args.push_back(const_cast<char*>(part.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        _exit(127);
    }
    res::set_active_child(child);  // an interrupted csyc stops the C compiler too
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        res::set_active_child(0);
        return -1;
    }
    res::set_active_child(0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

} // namespace

namespace {

// Mirrors the C struct emitted into every generated library.
struct EntryRow {
    const char* name;
    const char* sig;
    NativeEntry entry;
    int ret_float;
};
struct GlobalRow {
    const char* name;
    const char* sig;
    int user_nparams;
    const char* const* global_names;
    int global_count;
};

// Keys of the user-function instances this instance calls (final types).
std::vector<std::string> callee_instances(const InstMap& insts, const FnInfo& fn, bool& all_found) {
    std::vector<std::string> keys;
    all_found = true;
    all_stmt_exprs(fn.decl->body, [&](const ExprPtr& root) {
        return expr_tree_all(root, [&](const ExprPtr& n) {
            if (n->kind == ExprKind::Call && n->callee && n->callee->kind == ExprKind::Ident &&
                math_whitelist().count(n->callee->name) == 0) {
                std::string key;
                if (!callee_key(insts, fn, n, key) || !insts.count(key)) all_found = false;
                else keys.push_back(key);
            }
            return true;
        });
    });
    return keys;
}

#ifdef CSY_HAVE_X64_BACKEND
// Backend choice when a C compiler exists. The C path stays the default. The own x86-64 backend is
// preferred only for functions with a proven, profitable vectorisable loop: one whose element-wise
// fast path it emitted (the loop shape was proved safe, and a runtime check falls back to the scalar
// loop whenever the proof does not hold). Native-to-native calls stay inside one library or one image,
// so instances that call each other must share a backend: the unit is a connected component of the
// native call graph. A component qualifies when
//   1. at least one of its functions got a vector fast path, and
//   2. none of its functions is recursive (a call cycle: the own backend pays a full call sequence), and
//   3. every loop in it is a vector loop or contains one, so the time goes where the own backend wins.
// Measured on this machine: the own backend is 1.3x-2.6x faster than the C path
// on element-wise loops that read an array at several offsets or compute several operations, ties on
// pure bandwidth-bound ones, and is about 10% slower only when the vector path never engages
// (loops under 4 iterations, aliased arguments). Trip counts are unknown at compile time, so that
// last risk is accepted and documented. CSY_NATIVE_BACKEND=c / =x64 still force one backend.
std::unordered_set<std::string> own_backend_components(
        const InstMap& insts, const std::vector<std::string>& emit_order,
        const std::unordered_map<std::string, X64CodeGen::LoopStat>& stats) {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < emit_order.size(); i++) index.emplace(emit_order[i], i);
    const size_t n = emit_order.size();
    std::vector<std::vector<size_t>> out(n);
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; i++) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    for (size_t i = 0; i < n; i++) {
        bool all_found = true;
        for (const auto& callee : callee_instances(insts, insts.at(emit_order[i]), all_found)) {
            auto it = index.find(callee);
            if (it == index.end()) continue;
            out[i].push_back(it->second);
            parent[find(i)] = find(it->second);
        }
    }
    // call cycles (including a function calling itself), found with an iterative depth-first search
    std::vector<char> color(n, 0), cyclic_node(n, 0);
    for (size_t root = 0; root < n; root++) {
        if (color[root]) continue;
        std::vector<std::pair<size_t, size_t>> stack{{root, 0}};
        color[root] = 1;
        while (!stack.empty()) {
            auto& [node, next] = stack.back();
            if (next < out[node].size()) {
                const size_t to = out[node][next++];
                if (color[to] == 1) cyclic_node[to] = cyclic_node[node] = 1;
                else if (color[to] == 0) { color[to] = 1; stack.push_back({to, 0}); }
            } else { color[node] = 2; stack.pop_back(); }
        }
    }
    std::unordered_map<size_t, bool> has_vec, disqualified;
    for (size_t i = 0; i < n; i++) {
        const size_t c = find(i);
        auto it = stats.find(emit_order[i]);
        if (it == stats.end()) { disqualified[c] = true; continue; }     // unknown: keep the C path
        if (it->second.vector_loops > 0) has_vec[c] = true;
        if (it->second.other_loops > 0 || cyclic_node[i]) disqualified[c] = true;
    }
    std::unordered_set<std::string> chosen;
    for (size_t i = 0; i < n; i++) {
        const size_t c = find(i);
        if (has_vec[c] && !disqualified[c]) chosen.insert(emit_order[i]);
    }
    return chosen;
}
#endif

uint32_t sig_code(const std::string& sig) {
    uint32_t code = 0;
    for (size_t i = 0; i < sig.size(); i++) {
        const uint32_t c = sig[i] == 'I' ? 1u : sig[i] == 'F' ? 2u : 3u;
        code |= c << (2 * i);
    }
    return code;
}


constexpr size_t kMaxEmbeddedLibrary = 64u * 1024u * 1024u;

// Builds (or finds in the cache) the shared library for `c_source`. Returns its
// path, or "" with `why` set.
std::string build_library(const std::string& c_source, size_t variant_count, std::string& why) {
    const std::string dir = cache_dir();
    if (dir.empty()) { why = "cannot create a private native JIT cache directory"; return {}; }
    const char* cc_env = std::getenv("CC");
    std::vector<std::string> cmd;
    if (!cc_env || !*cc_env) cmd.emplace_back("cc");
    else if (!split_command_words(cc_env, cmd)) { why = "CC has unmatched quoting"; return {}; }
    // No -ffast-math and no FMA contraction: the native code must produce the
    // same floating-point results as the interpreter, bit for bit.
    const std::vector<std::string> flags = {"-O3", "-shared", "-fPIC", "-fno-math-errno", "-ffp-contract=off"};
    std::string key_material = c_source + "\n";
    // CSY_JIT_CACHE_SALT lets tests and benchmarks force a cold cache without
    // touching the shared cache directory.
    if (const char* salt = std::getenv("CSY_JIT_CACHE_SALT")) key_material += std::string("salt=") + salt + "\n";
    for (const auto& w : cmd) key_material += w + " ";
    for (const auto& f : flags) key_material += f + " ";
    // Is there a C compiler at all? Say so before doing any work (and before announcing a compile).
    {
        const std::string& prog = cmd.front();
        bool found = false;
        if (prog.find('/') != std::string::npos) found = ::access(prog.c_str(), X_OK) == 0;
        else if (const char* path = std::getenv("PATH")) {
            std::string p = path;
            size_t start = 0;
            while (start <= p.size() && !found) {
                size_t end = p.find(':', start);
                if (end == std::string::npos) end = p.size();
                const std::string candidate = (end > start ? p.substr(start, end - start) : std::string(".")) + "/" + prog;
                found = ::access(candidate.c_str(), X_OK) == 0;
                start = end + 1;
            }
        }
        if (!found) { why = "no C compiler ('" + prog + "') was found"; return {}; }
    }
    const std::string hash = hash_hex(key_material);
    const std::string so_path = dir + "/" + hash + ".so";
    const std::string c_path = dir + "/" + hash + ".c";

    bool cache_valid = false;
    if (path_exists(so_path)) {
        std::ifstream cached(c_path, std::ios::binary);
        if (cached) {
            std::ostringstream contents;
            contents << cached.rdbuf();
            cache_valid = contents.str() == c_source;
        }
        if (!cache_valid) ::unlink(so_path.c_str());
    }
    if (cache_valid) { dbg("using cached native build: " + so_path); return so_path; }

    if (!write_file_atomic(c_path, c_source)) { why = "failed to write generated C source to " + c_path; return {}; }
    fprintf(stderr, "[csy] compiling %zu native variant(s) (first run, cached after)...\n", variant_count);
    const std::string tmp_so = so_path + ".tmp" + std::to_string(::getpid());
    res::register_temp_path(tmp_so);
    std::vector<std::string> full = cmd;
    full.insert(full.end(), flags.begin(), flags.end());
    full.insert(full.end(), {"-o", tmp_so, c_path, "-lm"});
    const int rc = run_compiler(full, dir + "/" + hash + ".err");
    if (rc != 0 || ::rename(tmp_so.c_str(), so_path.c_str()) != 0) {
        ::unlink(tmp_so.c_str());
        res::unregister_temp_path(tmp_so);
        why = rc == 127 ? "no C compiler ('" + cmd.front() + "') was found"
                        : "native compile failed (rc=" + std::to_string(rc) + "), see " + dir + "/" + hash + ".err";
        return {};
    }
    res::unregister_temp_path(tmp_so);
    prune_cache(dir);
    return so_path;
}

// dlopens `so_path` and attaches its entry table to `ir.functions` by name.
bool bind_library(const std::string& so_path, IRModule& ir, int& loaded, std::string& why) {
    void* lib = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { why = std::string("dlopen failed: ") + dlerror(); return false; }
    const EntryRow* table = static_cast<const EntryRow*>(dlsym(lib, "csy_entry_table"));
    const int* count = static_cast<const int*>(dlsym(lib, "csy_entry_count"));
    const GlobalRow* global_table = static_cast<const GlobalRow*>(dlsym(lib, "csy_global_table"));
    const int* global_count = static_cast<const int*>(dlsym(lib, "csy_global_count"));
    if (!table || !count) { why = "compiled library has no entry table"; return false; }
    if ((global_table == nullptr) != (global_count == nullptr)) {
        why = "compiled library has incomplete module-global metadata";
        return false;
    }
    std::unordered_map<std::string, size_t> ir_index;
    for (size_t i = 0; i < ir.functions.size(); i++) ir_index.emplace(ir.functions[i].name, i);
    loaded = 0;
    for (int i = 0; i < *count; i++) {
        const EntryRow& row = table[i];
        auto it = ir_index.find(row.name);
        if (it == ir_index.end() || !row.entry || !row.sig || std::strlen(row.sig) > kMaxNativeParams) continue;
        const int sig_len = static_cast<int>(std::strlen(row.sig));
        int user_nparams = sig_len;
        const char* const* names = nullptr;
        int num_globals = 0;
        const bool has_global_metadata = global_table && global_count;
        if (has_global_metadata) {
            if (*global_count < 0 || *global_count > *count) continue;
            bool found_metadata = false;
            for (int g = 0; g < *global_count; g++) {
                if (global_table[g].name && global_table[g].sig &&
                    std::strcmp(global_table[g].name, row.name) == 0 &&
                    std::strcmp(global_table[g].sig, row.sig) == 0) {
                    user_nparams = global_table[g].user_nparams;
                    names = global_table[g].global_names;
                    num_globals = global_table[g].global_count;
                    found_metadata = true;
                    break;
                }
            }
            // New libraries publish one metadata row per native entry. If a
            // row is absent or malformed, skip it instead of binding a
            // hidden-argument entry as if it had the legacy user arity.
            if (!found_metadata) continue;
        }
        if (user_nparams < 0 || num_globals < 0 || num_globals > (int)kMaxNativeParams ||
            user_nparams + num_globals != sig_len || (num_globals > 0 && !names)) continue;
        NativeVariant v;
        v.entry = row.entry;
        v.sig = sig_code(row.sig);
        v.nparams = static_cast<uint8_t>(user_nparams);
        bool globals_found = true;
        for (int g = 0; g < num_globals; g++) {
            if (!names[g]) { globals_found = false; break; }
            auto gi = ir.global_index.find(names[g]);
            if (gi == ir.global_index.end()) { globals_found = false; break; }
            v.global_indices.push_back(gi->second);
        }
        if (!globals_found) continue;
        v.ret_float = row.ret_float != 0;
        ir.functions[it->second].native_variants.push_back(v);
        loaded++;
    }
    return true;
}


// A native function may only use its parameters and the locals it assigns itself. Reading a
// name it never assigns means reading a module-level variable, which native code cannot see;
// such a function must stay interpreted (and must not take the others down with it).
void collect_assigned_names(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        switch (s->kind) {
            case StmtKind::VarDecl:
            case StmtKind::ExprStmt:
                if (s->expr && s->expr->target && s->expr->target->kind == ExprKind::Ident) out.insert(s->expr->target->name);
                break;
            case StmtKind::If:
                collect_assigned_names(s->then_body, out);
                for (const auto& el : s->elifs) collect_assigned_names(el.second, out);
                collect_assigned_names(s->else_body, out);
                break;
            case StmtKind::While:
                collect_assigned_names(s->body, out);
                break;
            case StmtKind::For:
                out.insert(s->loop_var);
                collect_assigned_names(s->body, out);
                break;
            default:
                break;
        }
    }
}

// Definite-assignment analysis. Native code starts every local at 0 while the interpreter starts it
// at none, so a function is only native if every read of a local is preceded, on every path, by an
// assignment (parameters count as assigned). Reads of names the function never assigns are module-
// level variables, which native code cannot see. Loops are treated conservatively: what a loop body
// assigns is not assumed afterwards (the body may run zero times).
class AssignmentCheck {
public:
    using Set = std::unordered_set<std::string>;
    AssignmentCheck(const std::unordered_set<std::string>& assigned_anywhere,
                    const std::unordered_set<std::string>& module_globals,
                    const std::unordered_set<std::string>& numeric_globals)
        : anywhere_(assigned_anywhere), module_globals_(module_globals), numeric_globals_(numeric_globals) {}
    std::string reason() const { return reason_; }
    std::string bad_name() const { return bad_name_; }
    bool global_read() const { return global_; }
    const Set& global_reads() const { return global_reads_; }

    bool run(const Stmt* decl, const std::vector<std::string>& params) {
        Set st(params.begin(), params.end());
        bool reachable = true;
        return stmts(decl->body, st, reachable);
    }

private:
    const std::unordered_set<std::string>& anywhere_;
    const std::unordered_set<std::string>& module_globals_;
    const std::unordered_set<std::string>& numeric_globals_;
    std::string reason_, bad_name_;
    bool global_ = false;
    Set global_reads_;

    bool violation(const std::string& name) {
        if (bad_name_.empty()) { bad_name_ = name; global_ = !anywhere_.count(name); }
        return false;
    }
    bool expr(const ExprPtr& e, const Set& st) {
        if (!e) return true;
        switch (e->kind) {
            case ExprKind::Ident:
                if (st.count(e->name)) return true;
                if (module_globals_.count(e->name) && numeric_globals_.count(e->name)) {
                    global_reads_.insert(e->name);
                    return true;
                }
                return violation(e->name);
            case ExprKind::Call:
                for (const auto& a : e->args) if (!expr(a, st)) return false;
                return true;
            default:
                return expr(e->left, st) && expr(e->right, st) && expr(e->operand, st) && expr(e->index, st) &&
                       expr(e->object, st) && expr(e->value, st);
        }
    }
    static Set intersect(const Set& a, const Set& b) {
        Set out;
        for (const auto& n : a) if (b.count(n)) out.insert(n);
        return out;
    }
    // Analyses one branch from `entry`; returns the state at its end, or nullopt if the branch cannot fall through.
    bool branch(const std::vector<StmtPtr>& body, const Set& entry, bool& fall_through, Set& out) {
        out = entry;
        bool reachable = true;
        if (!stmts(body, out, reachable)) return false;
        fall_through = reachable;
        return true;
    }
    bool stmts(const std::vector<StmtPtr>& body, Set& st, bool& reachable) {
        for (const auto& s : body) {
            if (!s || !reachable) continue;
            switch (s->kind) {
                case StmtKind::VarDecl:
                case StmtKind::ExprStmt: {
                    const ExprPtr& e = s->expr;
                    if (!e) break;
                    if (!expr(e->value, st)) return false;
                    if (e->target) {
                        if (e->target->kind == ExprKind::Index) {
                            if (!expr(e->target->index, st) || !expr(e->target->object, st)) return false;
                        } else if (e->target->kind == ExprKind::Ident) {
                            const bool plain = s->kind == StmtKind::VarDecl || e->kind == ExprKind::Assign;
                            if (!plain && !st.count(e->target->name)) return violation(e->target->name);   // x += 1 reads x
                            st.insert(e->target->name);
                        }
                    } else if (e->kind != ExprKind::Assign && !expr(e, st)) {
                        return false;
                    }
                    break;
                }
                case StmtKind::If: {
                    if (!expr(s->cond, st)) return false;
                    const Set entry = st;
                    std::vector<Set> ends;
                    bool ft = true;
                    Set out;
                    if (!branch(s->then_body, entry, ft, out)) return false;
                    if (ft) ends.push_back(out);
                    for (const auto& el : s->elifs) {
                        if (!expr(el.first, entry)) return false;
                        if (!branch(el.second, entry, ft, out)) return false;
                        if (ft) ends.push_back(out);
                    }
                    if (!branch(s->else_body, entry, ft, out)) return false;   // no else: falls through with the entry state
                    if (ft) ends.push_back(out);
                    if (ends.empty()) { reachable = false; break; }
                    Set merged = ends[0];
                    for (size_t i = 1; i < ends.size(); i++) merged = intersect(merged, ends[i]);
                    st = std::move(merged);
                    break;
                }
                case StmtKind::While: {
                    if (!expr(s->cond, st)) return false;
                    bool ft = true; Set out;
                    if (!branch(s->body, st, ft, out)) return false;
                    break;
                }
                case StmtKind::For: {
                    if (s->iterable && !expr(s->iterable, st)) return false;
                    Set inner = st;
                    inner.insert(s->loop_var);
                    bool ft = true; Set out;
                    if (!branch(s->body, inner, ft, out)) return false;
                    break;
                }
                case StmtKind::Return:
                    if (!expr(s->ret_value, st)) return false;
                    reachable = false;
                    break;
                case StmtKind::Break:
                case StmtKind::Continue:
                    reachable = false;
                    break;
                default:
                    break;
            }
        }
        return true;
    }
};

// Returns a human-readable reason if the function may read a local before assigning it, or "".
std::string unsafe_read_reason(const Stmt* decl, const std::vector<std::string>& params,
                               const std::unordered_set<std::string>& module_globals,
                               const std::unordered_set<std::string>& numeric_globals,
                               std::unordered_set<std::string>& global_reads);

// Project builds mangle top-level names as "__csy_u<N>_<name>"; show the name the user wrote.

void collect_module_global_names(const std::vector<StmtPtr>& body,
                                 std::unordered_set<std::string>& names) {
    for (const auto& s : body) {
        if (!s || s->kind == StmtKind::FuncDecl) continue;
        if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->target) {
            if (s->expr->target->kind == ExprKind::Ident) {
                names.insert(s->expr->target->name);
            } else if (s->expr->target->kind == ExprKind::VectorLiteral) {
                for (const auto& element : s->expr->target->elements)
                    if (element && element->kind == ExprKind::Ident) names.insert(element->name);
            }
        }
        if (s->kind == StmtKind::If) {
            collect_module_global_names(s->then_body, names);
            for (const auto& branch : s->elifs) collect_module_global_names(branch.second, names);
            collect_module_global_names(s->else_body, names);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            collect_module_global_names(s->body, names);
        } else if (s->kind == StmtKind::Try) {
            collect_module_global_names(s->body, names);
            collect_module_global_names(s->then_body, names);
        }
    }
}

struct ModuleGlobalAssignments {
    bool has_plain_assignment = false;
    bool invalid_target = false;
    std::vector<ExprPtr> values;
};

void collect_module_global_assignments(const std::vector<StmtPtr>& body,
                                       const std::unordered_set<std::string>& globals,
                                       std::unordered_map<std::string, ModuleGlobalAssignments>& found) {
    for (const auto& s : body) {
        if (!s || s->kind == StmtKind::FuncDecl) continue;
        if ((s->kind == StmtKind::VarDecl || s->kind == StmtKind::ExprStmt) && s->expr && s->expr->target &&
            s->expr->target->kind == ExprKind::Ident && globals.count(s->expr->target->name)) {
            auto& item = found[s->expr->target->name];
            if (s->expr->kind == ExprKind::Assign) {
                item.has_plain_assignment = true;
                item.values.push_back(s->expr->value);
            } else if (s->expr->kind == ExprKind::CompoundAssign) {
                item.values.push_back(s->expr->value);
            } else {
                item.invalid_target = true;
            }
        }
        if (s->kind == StmtKind::If) {
            collect_module_global_assignments(s->then_body, globals, found);
            for (const auto& branch : s->elifs) collect_module_global_assignments(branch.second, globals, found);
            collect_module_global_assignments(s->else_body, globals, found);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            collect_module_global_assignments(s->body, globals, found);
        } else if (s->kind == StmtKind::Try) {
            collect_module_global_assignments(s->body, globals, found);
            collect_module_global_assignments(s->then_body, globals, found);
        }
    }
}

bool numeric_module_expr(const ExprPtr& e, const std::unordered_set<std::string>& possible_globals) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Int:
        case ExprKind::Float:
            return true;
        case ExprKind::Ident:
            return possible_globals.count(e->name) != 0;
        case ExprKind::Unary:
            return e->op == TokenType::MINUS && numeric_module_expr(e->operand, possible_globals);
        case ExprKind::Binary:
            return (e->op == TokenType::PLUS || e->op == TokenType::MINUS || e->op == TokenType::STAR ||
                    e->op == TokenType::SLASH || e->op == TokenType::PERCENT) &&
                   numeric_module_expr(e->left, possible_globals) &&
                   numeric_module_expr(e->right, possible_globals);
        case ExprKind::Call:
            if (!e->callee || e->callee->kind != ExprKind::Ident ||
                !math_whitelist().count(e->callee->name)) return false;
            return std::all_of(e->args.begin(), e->args.end(), [&](const ExprPtr& arg) {
                return numeric_module_expr(arg, possible_globals);
            });
        default:
            return false;
    }
}

std::unordered_set<std::string> numeric_module_globals(const std::vector<StmtPtr>& body,
                                                       const std::unordered_set<std::string>& globals) {
    std::unordered_map<std::string, ModuleGlobalAssignments> assignments;
    collect_module_global_assignments(body, globals, assignments);
    std::unordered_set<std::string> possible;
    for (const auto& name : globals) {
        auto it = assignments.find(name);
        if (it != assignments.end() && it->second.has_plain_assignment && !it->second.invalid_target)
            possible.insert(name);
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = possible.begin(); it != possible.end();) {
            const auto& item = assignments.at(*it);
            const bool valid = std::all_of(item.values.begin(), item.values.end(), [&](const ExprPtr& value) {
                return numeric_module_expr(value, possible);
            });
            if (!valid) { it = possible.erase(it); changed = true; }
            else ++it;
        }
    }
    return possible;
}

void collect_module_global_writes(const std::vector<StmtPtr>& body,
                                  const std::unordered_set<std::string>& globals,
                                  std::unordered_set<std::string>& writes) {
    for (const auto& s : body) {
        if (!s || s->kind == StmtKind::FuncDecl) continue;
        if ((s->kind == StmtKind::VarDecl || s->kind == StmtKind::ExprStmt) && s->expr &&
            s->expr->target && s->expr->target->kind == ExprKind::Ident &&
            globals.count(s->expr->target->name))
            writes.insert(s->expr->target->name);
        if (s->kind == StmtKind::If) {
            collect_module_global_writes(s->then_body, globals, writes);
            for (const auto& branch : s->elifs) collect_module_global_writes(branch.second, globals, writes);
            collect_module_global_writes(s->else_body, globals, writes);
        } else if (s->kind == StmtKind::While || s->kind == StmtKind::For || s->kind == StmtKind::Block) {
            collect_module_global_writes(s->body, globals, writes);
        } else if (s->kind == StmtKind::Try) {
            collect_module_global_writes(s->body, globals, writes);
            collect_module_global_writes(s->then_body, globals, writes);
        }
    }
}

std::string unsafe_module_write_reason(const Stmt* decl,
                                       const std::unordered_set<std::string>& globals) {
    std::unordered_set<std::string> writes;
    collect_module_global_writes(decl->body, globals, writes);
    // A parameter always shadows a module variable of the same name (it is a local),
    // so assigning one never writes module state.
    for (const auto& p : decl->params) writes.erase(p);
    if (writes.empty()) return {};
    const std::string name = user_visible_name(*std::min_element(writes.begin(), writes.end()));
    return "writes the module-level variable '" + name + "' (native code cannot update module state)";
}

std::string unsafe_read_reason(const Stmt* decl, const std::vector<std::string>& params,
                               const std::unordered_set<std::string>& module_globals,
                               const std::unordered_set<std::string>& numeric_globals,
                               std::unordered_set<std::string>& global_reads) {
    std::unordered_set<std::string> anywhere;
    collect_assigned_names(decl->body, anywhere);
    AssignmentCheck check(anywhere, module_globals, numeric_globals);
    const bool safe = check.run(decl, params);
    global_reads = check.global_reads();
    if (safe) return {};
    const std::string name = user_visible_name(check.bad_name());
    if (module_globals.count(check.bad_name()))
        return "reads the module-level variable '" + name + "' (not proven numeric at every module assignment site)";
    if (check.global_read())
        return "reads the module-level variable '" + name + "' (native functions see only their parameters and their own locals)";
    return "may read '" + name + "' before assigning it (native code starts locals at 0, the interpreter at none)";
}

// ============================================================
// Move 3: outline eligible top-level loops into synthetic functions
// ============================================================
//
// A `for`/`while` written directly in module code runs in the interpreter, and
// scripts put their hot loops there. This pass moves such a loop into a
// synthetic function `__loop_<line>` whose parameters are the module variables
// the loop needs, and replaces the loop with one call. The function then goes
// through the normal native pipeline (variants, both backends, --explain-native).
//
// The rewrite must not change what the program does, so a loop is outlined only
// when ALL of these hold (otherwise it stays in module code and the reason is
// recorded for --explain-native):
//   * the body is in the native numeric subset (checked with the real scanner,
//     with no user-function calls, so the loop cannot see or change module state
//     except through its own variables);
//   * every module variable it reads before setting, every array it uses, and
//     every variable it updates that is used afterwards was set before the loop;
//   * at most ONE variable it updates is used after the loop (it becomes the
//     return value); updated variables that are provably dead afterwards, and
//     loop temporaries nobody else mentions, become plain locals;
//   * no array is both read and written (so the loop can always be re-run from
//     the start after a native error and reproduce the interpreter's behaviour:
//     the VM does exactly that for `__loop_` functions).
// A module variable is "used afterwards" if any function mentions it or any later
// top-level statement mentions it, except when every later mention is a plain
// `x = ...;` that overwrites it first or a `for x in thru N` loop that does.
constexpr const char* kLoopFnPrefix = "__loop_";
constexpr size_t kMaxLiveOutputs = 3;     // variables used after an outlined loop (one native run each)

void mentions_stmts(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out);

void mentions_expr(const ExprPtr& e, std::unordered_set<std::string>& out) {
    if (!e) return;
    if (e->kind == ExprKind::Ident) out.insert(e->name);
    mentions_expr(e->left, out);
    mentions_expr(e->right, out);
    mentions_expr(e->operand, out);
    mentions_expr(e->target, out);
    mentions_expr(e->value, out);
    mentions_expr(e->callee, out);
    mentions_expr(e->object, out);
    mentions_expr(e->index, out);
    for (const auto& a : e->args) mentions_expr(a, out);
    for (const auto& a : e->elements) mentions_expr(a, out);
    for (const auto& p : e->dict_pairs) { mentions_expr(p.first, out); mentions_expr(p.second, out); }
}

void mentions_stmt(const StmtPtr& s, std::unordered_set<std::string>& out) {
    if (!s) return;
    mentions_expr(s->expr, out);
    mentions_expr(s->cond, out);
    mentions_expr(s->iterable, out);
    mentions_expr(s->ret_value, out);
    mentions_expr(s->using_expr, out);
    if (!s->loop_var.empty()) out.insert(s->loop_var);
    if (!s->catch_var.empty()) out.insert(s->catch_var);
    if (!s->using_alias.empty()) out.insert(s->using_alias);
    for (const auto& p : s->params) out.insert(p);
    mentions_stmts(s->body, out);
    mentions_stmts(s->then_body, out);
    mentions_stmts(s->else_body, out);
    for (const auto& el : s->elifs) { mentions_expr(el.first, out); mentions_stmts(el.second, out); }
}

void mentions_stmts(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out) {
    for (const auto& s : body) mentions_stmt(s, out);
}

void collect_for_loop_vars(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        if (s->kind == StmtKind::For) out.insert(s->loop_var);
        collect_for_loop_vars(s->body, out);
        collect_for_loop_vars(s->then_body, out);
        collect_for_loop_vars(s->else_body, out);
        for (const auto& el : s->elifs) collect_for_loop_vars(el.second, out);
    }
}

void collect_plain_assigned(const std::vector<StmtPtr>& body, std::unordered_set<std::string>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        if ((s->kind == StmtKind::VarDecl || s->kind == StmtKind::ExprStmt) && s->expr && s->expr->target) {
            if (s->expr->target->kind == ExprKind::Ident) out.insert(s->expr->target->name);
            else if (s->expr->target->kind == ExprKind::VectorLiteral)
                for (const auto& el : s->expr->target->elements)
                    if (el && el->kind == ExprKind::Ident) out.insert(el->name);
        }
        collect_plain_assigned(s->body, out);
        collect_plain_assigned(s->then_body, out);
        collect_plain_assigned(s->else_body, out);
        for (const auto& el : s->elifs) collect_plain_assigned(el.second, out);
    }
}

// True if `s` mentions `x` anywhere except inside a `for x in thru N` loop whose N does
// not mention x. Such a loop sets x before its body can read it, so it never observes a
// value that was left over from earlier code.
bool mentions_outside_for_var(const StmtPtr& s, const std::string& x) {
    if (!s) return false;
    if (s->kind == StmtKind::For && s->loop_var == x) {
        std::unordered_set<std::string> v;
        mentions_expr(s->iterable, v);
        return v.count(x) != 0;
    }
    std::unordered_set<std::string> m;
    mentions_expr(s->expr, m);
    mentions_expr(s->cond, m);
    mentions_expr(s->iterable, m);
    mentions_expr(s->ret_value, m);
    mentions_expr(s->using_expr, m);
    if (m.count(x) || s->loop_var == x || s->catch_var == x || s->using_alias == x) return true;
    for (const auto& p : s->params) if (p == x) return true;
    for (const auto& c : s->body) if (mentions_outside_for_var(c, x)) return true;
    for (const auto& c : s->then_body) if (mentions_outside_for_var(c, x)) return true;
    for (const auto& c : s->else_body) if (mentions_outside_for_var(c, x)) return true;
    for (const auto& el : s->elifs) {
        std::unordered_set<std::string> v;
        mentions_expr(el.first, v);
        if (v.count(x)) return true;
        for (const auto& c : el.second) if (mentions_outside_for_var(c, x)) return true;
    }
    return false;
}

// What a loop does with each name it mentions, in program order.
class LoopFacts {
public:
    std::vector<std::string> order;                     // first-occurrence order
    // 'R' read first (needs a value from before the loop), 'A' first set by an
    // unconditional statement of the loop body, 'C' first set conditionally,
    // 'S' first set as the variable of a nested `for` (only valid inside it).
    std::unordered_map<std::string, char> first;
    std::unordered_set<std::string> assigned, indexed, arr_written, arr_read;
    std::unordered_set<std::string> read_unsafe;        // read when it may not be set yet
    std::string bad;                                    // non-empty: cannot be outlined
    bool exits_early = false;                           // has its own `stop`/`next` (not inside a nested loop)

    void run(const StmtPtr& loop) {
        if (loop->kind == StmtKind::While) {
            expr(loop->cond);
        } else {
            for_header(*loop);
        }
        stmts(loop->body, true);
        if (loop->kind == StmtKind::For) scope_[loop->loop_var]--;
    }

private:
    std::unordered_map<std::string, int> scope_;        // nested-for variables currently in scope
    int nest_ = 0;                                      // depth of loops nested inside the outlined one

    void fail(const std::string& why) { if (bad.empty()) bad = why; }
    void read(const std::string& n) {
        auto it = first.find(n);
        if (it == first.end()) { order.push_back(n); first[n] = 'R'; return; }
        if (it->second == 'C' || (it->second == 'S' && scope_[n] == 0)) read_unsafe.insert(n);
    }
    void assign(const std::string& n, char kind) {
        assigned.insert(n);
        if (!first.count(n)) { order.push_back(n); first[n] = kind; }
    }
    // `for v in thru N`: N is read, then v is set once per iteration.
    void for_header(const Stmt& f) {
        const ExprPtr& it = f.iterable;
        if (!it || it->kind != ExprKind::Call || !it->callee || it->callee->kind != ExprKind::Ident ||
            it->callee->name != "range" || it->args.size() != 1) {
            fail("has a 'for' loop over something other than 'thru N' (e.g. a vector or string)");
            return;
        }
        expr(it->args[0]);
        assign(f.loop_var, 'S');
        scope_[f.loop_var]++;
    }
    void expr(const ExprPtr& e) {
        if (!e || !bad.empty()) return;
        switch (e->kind) {
            case ExprKind::Int:
            case ExprKind::Float:
                break;
            case ExprKind::Ident:
                read(e->name);
                break;
            case ExprKind::Index:
                if (!e->object || e->object->kind != ExprKind::Ident) { fail("indexes something that is not a plain variable"); return; }
                indexed.insert(e->object->name);
                arr_read.insert(e->object->name);
                read(e->object->name);
                expr(e->index);
                break;
            case ExprKind::Call:
                if (!e->callee || e->callee->kind != ExprKind::Ident) { fail("calls something that is not a plain function name"); return; }
                if (!math_whitelist().count(e->callee->name)) {
                    fail("calls '" + user_visible_name(e->callee->name) +
                         "' (only math builtins are allowed in an outlined loop for now)");
                    return;
                }
                for (const auto& a : e->args) expr(a);
                break;
            case ExprKind::Binary:
                expr(e->left);
                expr(e->right);
                break;
            case ExprKind::Unary:
                expr(e->operand);
                break;
            case ExprKind::String: fail("uses a string"); break;
            case ExprKind::Bool:
            case ExprKind::None: fail("uses a bool or none value"); break;
            case ExprKind::VectorLiteral: fail("uses a vector literal"); break;
            case ExprKind::DictLiteral: fail("uses a dictionary"); break;
            case ExprKind::Member: fail("uses member access (bc::name or object.field)"); break;
            default: fail("uses an expression the native subset does not support"); break;
        }
    }
    void assign_expr(const ExprPtr& a, bool top) {
        if (!a->target) { fail("has an assignment without a target"); return; }
        const bool compound = a->kind == ExprKind::CompoundAssign;
        const ExprPtr& t = a->target;
        if (t->kind == ExprKind::Ident) {
            if (compound) read(t->name);
            expr(a->value);
            assign(t->name, top ? 'A' : 'C');
        } else if (t->kind == ExprKind::Index && t->object && t->object->kind == ExprKind::Ident) {
            const std::string& name = t->object->name;
            indexed.insert(name);
            arr_written.insert(name);
            if (compound) arr_read.insert(name);
            read(name);
            expr(t->index);
            expr(a->value);
        } else {
            fail("assigns to something other than a plain number variable or an f64_array element");
        }
    }
    void stmts(const std::vector<StmtPtr>& body, bool top) {
        for (const auto& s : body) {
            if (!s || !bad.empty()) return;
            switch (s->kind) {
                case StmtKind::VarDecl:
                    if (!s->expr || s->expr->kind != ExprKind::Assign) { fail("declaration that is not a plain assignment"); return; }
                    assign_expr(s->expr, top);
                    break;
                case StmtKind::ExprStmt:
                    if (s->expr && (s->expr->kind == ExprKind::CompoundAssign || s->expr->kind == ExprKind::Assign))
                        assign_expr(s->expr, top);
                    else fail("has a bare call or output statement (e.g. bc::out, push): side effects stay interpreted");
                    break;
                case StmtKind::If:
                    expr(s->cond);
                    stmts(s->then_body, false);
                    for (const auto& el : s->elifs) { expr(el.first); stmts(el.second, false); }
                    stmts(s->else_body, false);
                    break;
                case StmtKind::While:
                    expr(s->cond);
                    nest_++;
                    stmts(s->body, false);
                    nest_--;
                    break;
                case StmtKind::For:
                    for_header(*s);
                    nest_++;
                    stmts(s->body, false);
                    nest_--;
                    scope_[s->loop_var]--;
                    break;
                case StmtKind::Break:
                case StmtKind::Continue:
                    if (nest_ == 0) exits_early = true;
                    break;
                case StmtKind::Return:
                    fail("contains 'rt', which would end the whole program");
                    return;
                default:
                    fail("contains a nested function, try/catch, or other statement the native subset lacks");
                    return;
            }
        }
    }
};

// Deep copy of a loop with some identifiers renamed (variables only; function names and
// builtin names are never in `ren`). The original tree is left untouched.
using RenameMap = std::unordered_map<std::string, std::string>;

ExprPtr clone_expr(const ExprPtr& e, const RenameMap& ren) {
    if (!e) return nullptr;
    auto c = std::make_shared<Expr>(*e);
    if (c->kind == ExprKind::Ident) {
        auto it = ren.find(c->name);
        if (it != ren.end()) c->name = it->second;
    }
    c->left = clone_expr(e->left, ren);
    c->right = clone_expr(e->right, ren);
    c->operand = clone_expr(e->operand, ren);
    c->target = clone_expr(e->target, ren);
    c->value = clone_expr(e->value, ren);
    c->callee = clone_expr(e->callee, ren);
    c->object = clone_expr(e->object, ren);
    c->index = clone_expr(e->index, ren);
    for (auto& a : c->args) a = clone_expr(a, ren);
    for (auto& a : c->elements) a = clone_expr(a, ren);
    for (auto& p : c->dict_pairs) { p.first = clone_expr(p.first, ren); p.second = clone_expr(p.second, ren); }
    return c;
}

StmtPtr clone_stmt(const StmtPtr& s, const RenameMap& ren);

std::vector<StmtPtr> clone_stmts(const std::vector<StmtPtr>& v, const RenameMap& ren) {
    std::vector<StmtPtr> out;
    out.reserve(v.size());
    for (const auto& s : v) out.push_back(clone_stmt(s, ren));
    return out;
}

StmtPtr clone_stmt(const StmtPtr& s, const RenameMap& ren) {
    if (!s) return nullptr;
    auto c = std::make_shared<Stmt>(*s);
    c->using_expr = clone_expr(s->using_expr, ren);
    c->expr = clone_expr(s->expr, ren);
    c->cond = clone_expr(s->cond, ren);
    c->iterable = clone_expr(s->iterable, ren);
    c->ret_value = clone_expr(s->ret_value, ren);
    c->body = clone_stmts(s->body, ren);
    c->then_body = clone_stmts(s->then_body, ren);
    c->else_body = clone_stmts(s->else_body, ren);
    for (auto& el : c->elifs) { el.first = clone_expr(el.first, ren); el.second = clone_stmts(el.second, ren); }
    if (!c->loop_var.empty()) {
        auto it = ren.find(c->loop_var);
        if (it != ren.end()) c->loop_var = it->second;
    }
    return c;
}

class LoopOutliner {
public:
    explicit LoopOutliner(Module& mod) : mod_(mod), body_(mod.body) {}

    void run() {
        bool any = false;
        for (const auto& s : body_)
            if (s && (s->kind == StmtKind::For || s->kind == StmtKind::While)) { any = true; break; }
        if (!any) return;
        index_module();
        std::vector<StmtPtr> rewritten;
        rewritten.reserve(body_.size() + 8);
        for (size_t j = 0; j < body_.size(); j++) {
            const StmtPtr& s = body_[j];
            if (s && (s->kind == StmtKind::For || s->kind == StmtKind::While)) {
                std::vector<StmtPtr> emitted;
                if (plan(j, emitted)) {
                    for (auto& e : emitted) rewritten.push_back(std::move(e));
                    continue;
                }
            }
            rewritten.push_back(s);
        }
        body_ = std::move(rewritten);
    }

private:
    Module& mod_;
    std::vector<StmtPtr>& body_;
    std::unordered_map<std::string, std::vector<uint32_t>> mention_index_;  // non-function statements
    std::unordered_map<std::string, std::vector<uint32_t>> def_index_;      // straight-line `x = ...;`
    std::unordered_set<std::string> fn_names_, fn_touched_;

    void index_module() {
        for (size_t j = 0; j < body_.size(); j++) {
            const StmtPtr& s = body_[j];
            if (!s) continue;
            if (s->kind == StmtKind::FuncDecl) {
                fn_names_.insert(s->func_name);
                add_touched(*s);
                continue;
            }
            std::unordered_set<std::string> m;
            mentions_stmt(s, m);
            for (const auto& name : m) mention_index_[name].push_back(static_cast<uint32_t>(j));
            if (s->kind == StmtKind::VarDecl && s->expr && s->expr->kind == ExprKind::Assign && s->expr->target) {
                if (s->expr->target->kind == ExprKind::Ident) {
                    def_index_[s->expr->target->name].push_back(static_cast<uint32_t>(j));
                } else if (s->expr->target->kind == ExprKind::VectorLiteral) {
                    for (const auto& el : s->expr->target->elements)
                        if (el && el->kind == ExprKind::Ident) def_index_[el->name].push_back(static_cast<uint32_t>(j));
                }
            }
        }
    }

    // Names a function may read or write as MODULE variables: everything it mentions
    // except its parameters and its own `for` variables (a `for` variable is always a local).
    void add_touched(const Stmt& f) {
        std::unordered_set<std::string> m, for_vars, assigned;
        mentions_stmts(f.body, m);
        collect_for_loop_vars(f.body, for_vars);
        collect_plain_assigned(f.body, assigned);
        for (const auto& p : f.params) m.erase(p);
        for (const auto& v : for_vars)
            if (!assigned.count(v)) m.erase(v);
        fn_touched_.insert(m.begin(), m.end());
    }

    bool used_outside(const std::string& name, size_t k) const {
        if (fn_touched_.count(name)) return true;
        auto it = mention_index_.find(name);
        if (it == mention_index_.end()) return false;
        for (uint32_t idx : it->second) if (idx != k) return true;
        return false;
    }
    bool defined_before(const std::string& name, size_t k) const {
        auto it = def_index_.find(name);
        return it != def_index_.end() && !it->second.empty() && it->second.front() < k;
    }
    // True when nothing can observe `name`'s value after the loop at index k.
    bool dead_after(const std::string& name, size_t k) const {
        if (fn_touched_.count(name)) return false;
        auto it = mention_index_.find(name);
        if (it == mention_index_.end()) return true;
        for (uint32_t idx : it->second) {
            if (idx <= k) continue;
            const Stmt& s = *body_[idx];
            if (s.kind == StmtKind::VarDecl && s.expr && s.expr->kind == ExprKind::Assign && s.expr->target &&
                s.expr->target->kind == ExprKind::Ident && s.expr->target->name == name) {
                std::unordered_set<std::string> v;
                mentions_expr(s.expr->value, v);
                return !v.count(name);       // overwritten before anything can read it
            }
            // Every mention is inside a `for name in thru N` that sets it first: nothing can read
            // the loop's leftover value (if such a loop runs zero times the value just stays
            // stale, and the scan continues with the statements after it).
            if (mentions_outside_for_var(body_[idx], name)) return false;
        }
        return true;
    }

    // ---- parameter types, guessed from straight-line module code (a hint only:
    // a wrong guess just adds an unused native variant, it can never change results) ----
    char guess_type(const std::string& name, size_t k, int depth) const {
        if (depth > 8 || fn_touched_.count(name)) return 0;
        auto dit = def_index_.find(name);
        if (dit == def_index_.end()) return 0;
        size_t di = SIZE_MAX;
        for (uint32_t idx : dit->second) if (idx < k) di = idx;
        if (di == SIZE_MAX) return 0;
        auto mit = mention_index_.find(name);
        if (mit != mention_index_.end())
            for (uint32_t idx : mit->second) if (idx > di && idx < k) return 0;
        const Stmt& def = *body_[di];
        if (!def.expr || !def.expr->target || def.expr->target->kind != ExprKind::Ident) return 0;
        return guess_expr(def.expr->value, di, depth + 1);
    }
    char guess_expr(const ExprPtr& e, size_t at, int depth) const {
        if (!e) return 0;
        switch (e->kind) {
            case ExprKind::Int: return 'I';
            case ExprKind::Float: return 'F';
            case ExprKind::Ident: return guess_type(e->name, at, depth);
            case ExprKind::Unary: return e->op == TokenType::MINUS ? guess_expr(e->operand, at, depth) : 0;
            case ExprKind::Binary: {
                const char a = guess_expr(e->left, at, depth), b = guess_expr(e->right, at, depth);
                if (!a || !b) return 0;
                if (e->op == TokenType::PLUS || e->op == TokenType::MINUS || e->op == TokenType::STAR ||
                    e->op == TokenType::SLASH)
                    return (a == 'F' || b == 'F') ? 'F' : 'I';
                if (e->op == TokenType::PERCENT) return (a == 'I' && b == 'I') ? 'I' : 0;
                return 0;
            }
            case ExprKind::Call: {
                if (!e->callee || e->callee->kind != ExprKind::Ident) return 0;
                auto mit = math_whitelist().find(e->callee->name);
                if (mit == math_whitelist().end()) return 0;
                if (mit->second.kind != MathKind::Promote2) return 'F';
                if (e->args.size() != 2) return 0;
                const char a = guess_expr(e->args[0], at, depth), b = guess_expr(e->args[1], at, depth);
                if (!a || !b) return 0;
                return (a == 'F' || b == 'F') ? 'F' : 'I';
            }
            default: return 0;
        }
    }

    std::string unique_name(int line) {
        std::string nm = std::string(kLoopFnPrefix) + std::to_string(line);
        int seq = 1;
        while (fn_names_.count(nm) || mention_index_.count(nm) || fn_touched_.count(nm))
            nm = std::string(kLoopFnPrefix) + std::to_string(line) + "_" + std::to_string(++seq);
        fn_names_.insert(nm);
        return nm;
    }

    // The literal (int or float) that the loop body's first statement for `name` assigns, if that
    // statement is a plain `name = <literal>;` directly in the body.
    static ExprPtr first_assigned_literal(const Stmt& loop, const std::string& name) {
        for (const auto& st : loop.body) {
            if (!st || st->kind != StmtKind::VarDecl || !st->expr || st->expr->kind != ExprKind::Assign ||
                !st->expr->target || st->expr->target->kind != ExprKind::Ident || st->expr->target->name != name)
                continue;
            const ExprPtr& v = st->expr->value;
            return (v && (v->kind == ExprKind::Int || v->kind == ExprKind::Float)) ? v : nullptr;
        }
        return nullptr;
    }

    // `for v in thru K` with an integer literal K >= 1 always executes its body.
    static bool runs_at_least_once(const Stmt& loop) {
        if (loop.kind != StmtKind::For || !loop.iterable || loop.iterable->args.size() != 1) return false;
        const ExprPtr& bound = loop.iterable->args[0];
        return bound && bound->kind == ExprKind::Int && bound->int_val >= 1;
    }

    bool keep(const StmtPtr& loop, const std::string& why) {
        LoopOutlineNote note;
        note.line = loop->tok.line;
        note.detail = why;
        note.original = loop;
        mod_.loop_notes.push_back(std::move(note));
        return false;
    }

    bool plan(size_t k, std::vector<StmtPtr>& emitted) {
        const StmtPtr loop = body_[k];
        LoopFacts facts;
        facts.run(loop);
        if (!facts.bad.empty()) return keep(loop, facts.bad);
        for (const auto& a : facts.arr_written)
            if (facts.arr_read.count(a))
                return keep(loop, "reads and writes the array '" + user_visible_name(a) +
                                  "' in the same loop (after a native error it could not be re-run safely)");

        std::vector<std::string> params, live;
        std::vector<bool> scalar_arg;
        RenameMap rename;       // loop-local names that are also module variables elsewhere
        std::unordered_map<std::string, char> seeds;   // live locals that get an unobservable starting value
        std::unordered_set<std::string> param_set;
        for (const std::string& name : facts.order) {
            const std::string shown = user_visible_name(name);
            if (fn_names_.count(name)) return keep(loop, "uses the function '" + shown + "' as a value");
            const bool is_array = facts.indexed.count(name) != 0;
            const bool assigned = facts.assigned.count(name) != 0;
            if (is_array && assigned) return keep(loop, "uses '" + shown + "' both as an array and as a number");
            const char fk = facts.first.at(name);
            const bool needs_init = fk == 'R' || facts.read_unsafe.count(name) != 0;
            const bool outside = used_outside(name, k);
            bool as_param = false;
            if (is_array) {
                as_param = true;
            } else if (!outside) {
                if (needs_init) return keep(loop, "reads '" + shown + "' before it is set");
            } else if (!assigned) {
                as_param = true;                         // module variable the loop only reads
            } else if (!dead_after(name, k)) {
                // Updated here and used later: returned. If the loop must run at least once and sets
                // the variable first thing in its body, the value from before the loop can never
                // be observed, so it need not exist (or be passed in).
                as_param = true;
                if (fk == 'A' && runs_at_least_once(*loop) && !facts.exits_early) {
                    const ExprPtr seed = first_assigned_literal(*loop, name);
                    if (seed) { as_param = false; seeds[name] = seed->kind == ExprKind::Int ? 'I' : 'F'; }
                }
                live.push_back(name);
            } else {
                as_param = needs_init;                   // updated here, never observed later
            }
            if (as_param && !defined_before(name, k))
                return keep(loop, "uses '" + shown + "', which is not set by a plain assignment before the loop");
            if (as_param) { params.push_back(name); param_set.insert(name); scalar_arg.push_back(!is_array); }
            else if (outside) {
                // Not a parameter, yet the module also has a variable of this name: inside a function
                // an assignment would write that module variable (Move 0), so the function body
                // uses a private name for it instead.
                std::string fresh = name + "__L" + std::to_string(loop->tok.line);
                while (mention_index_.count(fresh) || fn_names_.count(fresh) || fn_touched_.count(fresh) ||
                       facts.first.count(fresh))
                    fresh += "_";
                rename[name] = fresh;
            }
        }
        // Each variable used afterwards gets its own function that runs the whole loop and returns that
        // variable (no change to the native calling convention). The loop is deterministic and its array
        // writes are idempotent (no array is both read and written), so repeating it is safe; the cost is
        // one more native run per extra variable, hence the cap.
        if (live.size() > kMaxLiveOutputs) {
            std::string names;
            for (const auto& n : live) names += (names.empty() ? "" : ", ") + user_visible_name(n);
            return keep(loop, "updates " + std::to_string(live.size()) + " variables that are used after the loop (" +
                              names + "); at most " + std::to_string(kMaxLiveOutputs) +
                              " are supported (each costs one more native run of the loop)");
        }
        if (params.size() > kMaxNativeParams)
            return keep(loop, "needs more than " + std::to_string(kMaxNativeParams) + " variables");

        const Token tok = loop->tok;
        const size_t outputs = live.empty() ? 1 : live.size();
        std::vector<StmtPtr> fn_stmts;
        std::vector<std::string> fn_list;
        auto release_names = [&]() { for (const auto& n : fn_list) fn_names_.erase(n); };
        const std::unordered_map<std::string, FnInfo> no_candidates;
        for (size_t oi = 0; oi < outputs; oi++) {
            const std::string ret_name = live.empty() ? std::string() : live[oi];
            auto fn = make_stmt(StmtKind::FuncDecl, tok);
            fn->func_name = unique_name(tok.line);
            fn_list.push_back(fn->func_name);
            fn->params = params;
            // The loop is certain to run and sets these first thing, so these starting values are never
            // read; they only let the native checker see the variable is always set before `rt`.
            // Only the variable this function returns needs one.
            for (const auto& sd : seeds) {
                if (sd.first != ret_name) continue;
                auto target = make_expr(ExprKind::Ident, tok);
                auto rit = rename.find(sd.first);
                target->name = rit != rename.end() ? rit->second : sd.first;
                auto lit = make_expr(sd.second == 'I' ? ExprKind::Int : ExprKind::Float, tok);
                lit->int_val = 0;
                lit->float_val = 0.0;
                auto assign = make_expr(ExprKind::Assign, tok);
                assign->target = target;
                assign->value = lit;
                assign->depth = 3;
                auto seed_stmt = make_stmt(StmtKind::VarDecl, tok);
                seed_stmt->expr = assign;
                fn->body.push_back(seed_stmt);
            }
            fn->body.push_back((rename.empty() && outputs == 1) ? loop : clone_stmt(loop, rename));
            auto ret = make_stmt(StmtKind::Return, tok);
            if (!live.empty()) {
                auto id = make_expr(ExprKind::Ident, tok);
                auto rit = rename.find(ret_name);
                id->name = rit != rename.end() ? rit->second : ret_name;
                ret->ret_value = id;
            } else {
                auto zero = make_expr(ExprKind::Int, tok);
                zero->int_val = 0;
                ret->ret_value = zero;
            }
            fn->body.push_back(ret);

            // The real native scanner is the gate: if it would refuse this function, outlining gains nothing.
            FnInfo info;
            info.decl = fn.get();
            info.param_names = params;
            info.user_param_count = params.size();
            std::unordered_set<std::string> pset(params.begin(), params.end());
            find_array_params_stmts(fn->body, pset, info.array_params);
            EligibilityScanner scanner(no_candidates);
            if (!scanner.scan(info)) {
                release_names();
                return keep(loop, scanner.why());
            }
            fn_stmts.push_back(fn);
        }

        std::string hint;
        for (size_t i = 0; i < params.size(); i++) {
            const char c = scalar_arg[i] ? guess_type(params[i], k, 0) : 'A';
            if (!c) { hint.clear(); break; }
            hint += c;
        }

        auto make_call = [&](const std::string& fname) {
            auto callee = make_expr(ExprKind::Ident, tok);
            callee->name = fname;
            auto call = make_expr(ExprKind::Call, tok);
            call->callee = callee;
            call->depth = 2;
            for (const auto& p : params) {
                auto arg = make_expr(ExprKind::Ident, tok);
                arg->name = p;
                call->args.push_back(arg);
            }
            return call;
        };
        auto make_assign = [&](const std::string& target_name, const ExprPtr& value) {
            auto target = make_expr(ExprKind::Ident, tok);
            target->name = target_name;
            auto assign = make_expr(ExprKind::Assign, tok);
            assign->target = target;
            assign->value = value;
            assign->depth = 3;
            auto st = make_stmt(StmtKind::VarDecl, tok);
            st->expr = assign;
            return st;
        };
        std::vector<StmtPtr> call_stmts;
        if (live.empty()) {
            auto st = make_stmt(StmtKind::ExprStmt, tok);
            st->expr = make_call(fn_list[0]);
            call_stmts.push_back(st);
        } else if (live.size() == 1) {
            call_stmts.push_back(make_assign(live[0], make_call(fn_list[0])));
        } else {
            // Every call must see the values from BEFORE the loop, so the results go into temporaries
            // first and are only then assigned to the real variables.
            std::vector<std::string> temps;
            for (size_t oi = 0; oi < live.size(); oi++) {
                std::string tmp = "__lv_" + std::to_string(tok.line) + "_" + std::to_string(oi);
                while (mention_index_.count(tmp) || fn_names_.count(tmp) || fn_touched_.count(tmp)) tmp += "_";
                fn_names_.insert(tmp);                        // reserve the name
                temps.push_back(tmp);
                call_stmts.push_back(make_assign(tmp, make_call(fn_list[oi])));
            }
            for (size_t oi = 0; oi < live.size(); oi++) {
                auto id = make_expr(ExprKind::Ident, tok);
                id->name = temps[oi];
                call_stmts.push_back(make_assign(live[oi], id));
            }
        }
        for (auto& f : fn_stmts) emitted.push_back(f);
        for (auto& c : call_stmts) emitted.push_back(c);

        LoopOutlineNote note;
        note.line = tok.line;
        note.outlined = true;
        note.fns = fn_list;
        if (live.empty()) {
            note.detail = "outlined; updates no variable used afterwards";
        } else {
            std::string names;
            for (const auto& n : live) names += (names.empty() ? "" : ", ") + user_visible_name(n);
            note.detail = "outlined; returns " + names + (live.size() > 1 ? " (one native run per variable)" : "");
        }
        note.scalar_arg = scalar_arg;
        note.sig_hint = hint;
        note.original = loop;
        note.call_stmts = call_stmts;
        mod_.loop_notes.push_back(std::move(note));
        return true;
    }
};

NativeReport try_native_compile_impl(const Module& mod, IRModule& ir, NativeMode mode, bool embed_library) {
    NativeReport report;
    if (std::getenv("CSY_NO_NATIVE_JIT")) {
        dbg("CSY_NO_NATIVE_JIT set, skipping entirely");
        report.status = "native fast path disabled (CSY_NO_NATIVE_JIT is set)";
        return report;
    }

    // ---- Candidates: top-level functions only (class methods carry a mangled
    // "$Class$method" name and an implicit `me` parameter). ----
    InstMap fns;
    std::vector<std::string> decl_order;
    for (const auto& s : mod.body) {
        if (s->kind != StmtKind::FuncDecl) continue;
        if (s->func_name.empty()) continue;
        if (s->func_name[0] == '$') { report.interpreted_only++; continue; }
        FnInfo info;
        info.decl = s.get();
        info.param_names = s->params;
        info.user_param_count = s->params.size();
        std::unordered_set<std::string> params(s->params.begin(), s->params.end());
        find_array_params_stmts(s->body, params, info.array_params);
        fns.emplace(s->func_name, std::move(info));
        decl_order.push_back(s->func_name);
    }
    for (const auto& s : mod.body)
        if (s && (s->kind == StmtKind::For || s->kind == StmtKind::While)) report.top_level_loops++;
    for (const auto& note : mod.loop_notes) {
        if (note.outlined) continue;
        NativeFunctionReport kept;
        kept.line = note.line;
        kept.detail = note.detail;
        report.kept_loops.push_back(std::move(kept));
    }
    // Module-variable analysis looks at the program as written: an outlined loop is
    // put back in place of its call, so every proof below is exactly the pre-outlining one.
    std::vector<StmtPtr> analysis_storage;
    const std::vector<StmtPtr>* analysis_body = &mod.body;
    {
        bool any_outlined = false;
        for (const auto& note : mod.loop_notes) if (note.outlined) { any_outlined = true; break; }
        if (any_outlined) {
            // The first statement that replaced a loop stands for the original loop; the others vanish.
            std::unordered_map<const Stmt*, StmtPtr> original_of;
            std::unordered_set<const Stmt*> dropped;
            for (const auto& note : mod.loop_notes) {
                if (!note.outlined || !note.original || note.call_stmts.empty()) continue;
                original_of[note.call_stmts[0].get()] = note.original;
                for (size_t ci = 1; ci < note.call_stmts.size(); ci++) dropped.insert(note.call_stmts[ci].get());
            }
            for (const auto& st : mod.body) {
                if (dropped.count(st.get())) continue;
                auto it = original_of.find(st.get());
                analysis_storage.push_back(it != original_of.end() ? it->second : st);
            }
            analysis_body = &analysis_storage;
        }
    }
    // Real module variables (after outlining) decide what a function may read or write;
    // the as-written set only feeds the numeric proof, so loop temporaries (which are now
    // plain locals) are not mistaken for module state.
    std::unordered_set<std::string> module_globals, proof_globals;
    collect_module_global_names(mod.body, module_globals);
    collect_module_global_names(*analysis_body, proof_globals);
    const std::unordered_set<std::string> numeric_globals = numeric_module_globals(*analysis_body, proof_globals);
    std::unordered_map<std::string, int> fn_line;
    for (const auto& s : mod.body)
        if (s && s->kind == StmtKind::FuncDecl && !s->func_name.empty())
            fn_line.emplace(s->func_name, s->tok.line);
    if (fns.empty()) {
        report.status = "no top-level functions";
        return report;
    }

    // ---- Stage 1: eligibility (syntax) ----
    EligibilityScanner scanner(fns);
    for (auto& [name, fn] : fns) {
        if (fn.param_names.size() > kMaxNativeParams) {
            fn.why_syntax = "has more than " + std::to_string(kMaxNativeParams) + " parameters";
        } else if (!scanner.scan(fn)) {
            fn.why_syntax = scanner.why();
        } else if (has_bare_return(fn.decl->body)) {
            fn.why_syntax = "has a bare 'rt;' (returns none)";
        } else if (const std::string global_write = unsafe_module_write_reason(fn.decl, module_globals); !global_write.empty()) {
            fn.why_syntax = global_write;
        } else if (const std::string unsafe = unsafe_read_reason(fn.decl, fn.param_names,
                                                                  module_globals, numeric_globals,
                                                                  fn.direct_global_reads); !unsafe.empty()) {
            fn.why_syntax = unsafe;
        } else {
            fn.global_params.assign(fn.direct_global_reads.begin(), fn.direct_global_reads.end());
            std::sort(fn.global_params.begin(), fn.global_params.end());
            fn.syntax_ok = true;
        }
    }
    std::unordered_set<std::string> eligible;
    for (auto& [name, fn] : fns) if (fn.syntax_ok) eligible.insert(name);
    bool eligibility_changed = true;
    while (eligibility_changed) {
        eligibility_changed = false;
        for (const auto& name : eligible) {
            FnInfo& fn = fns.at(name);
            std::unordered_set<std::string> reads = fn.direct_global_reads;
            for (const auto& dep : fn.calls_candidates) {
                if (!eligible.count(dep)) continue;
                const auto& dep_reads = fns.at(dep).global_params;
                reads.insert(dep_reads.begin(), dep_reads.end());
            }
            std::vector<std::string> sorted(reads.begin(), reads.end());
            std::sort(sorted.begin(), sorted.end());
            if (sorted != fn.global_params) { fn.global_params = std::move(sorted); eligibility_changed = true; }
        }
        for (auto it = eligible.begin(); it != eligible.end();) {
            FnInfo& fn = fns.at(*it);
            if (fn.user_param_count + fn.global_params.size() > kMaxNativeParams) {
                fn.why_syntax = "needs more than " + std::to_string(kMaxNativeParams) +
                                " native parameters after passing module globals";
                it = eligible.erase(it);
                eligibility_changed = true;
                continue;
            }
            std::string bad_dep;
            for (const auto& dep : fn.calls_candidates) if (!eligible.count(dep)) { bad_dep = dep; break; }
            if (!bad_dep.empty()) {
                fn.why_syntax = "calls '" + bad_dep + "', which cannot be compiled natively" +
                                (fns.at(bad_dep).why_syntax.empty() ? "" : " (" + fns.at(bad_dep).why_syntax + ")");
                dbg("'" + *it + "' disqualified: calls a function that isn't itself eligible");
                it = eligible.erase(it);
                eligibility_changed = true;
            } else {
                ++it;
            }
        }
    }
    for (const auto& name : eligible)
        for (const auto& global : fns.at(name).global_params)
            fns.at(name).param_names.push_back(global);
    for (const auto& name : decl_order) {
        if (!fns.at(name).syntax_ok) {
            dbg("'" + name + "' not eligible for native compilation: " + fns.at(name).why_syntax);
        }
    }
    auto fill_report = [&](const std::unordered_map<std::string, std::string>& compiled_detail,
                           const std::function<std::string(const std::string&)>& variant_reason) {
        for (const auto& name : decl_order) {
            NativeFunctionReport fr;
            fr.name = name;
            fr.line = fn_line.count(name) ? fn_line.at(name) : 0;
            auto cd = compiled_detail.find(name);
            if (cd != compiled_detail.end()) { fr.native = true; fr.detail = cd->second; }
            else if (!fns.at(name).syntax_ok || !eligible.count(name)) fr.detail = fns.at(name).why_syntax;
            else fr.detail = variant_reason(name);
            if (fr.detail.empty() && !fr.native) fr.detail = "not compiled";
            report.functions.push_back(std::move(fr));
        }
    };
    if (eligible.empty()) {
        dbg("no functions eligible for native compilation");
        fill_report({}, [](const std::string&) { return std::string(); });
        report.status = "no function is eligible for native compilation";
        return report;
    }
    InstMap bases;
    for (const auto& name : eligible) { bases.emplace(name, fns.at(name)); bases.at(name).eligible = true; }

    // ---- Stage 2: specialisation + type inference ----
    // Every eligible function gets a variant for each plausible way the
    // interpreter may call it (all-int; and for few parameters every int/float
    // combination), plus whatever variants native callers need - chosen from the
    // static types of their arguments. A function called with floats where its
    // body "looks" integer is therefore no longer a dead end.
    InstMap insts;
    std::vector<std::string> creation_order;
    TypeInferer inferer(insts, bases, creation_order);
    std::unordered_set<std::string> root_keys;
    auto root_signature = [&](const FnInfo& base, unsigned mask) {
        std::string sig;
        int bit = 0;
        for (const auto& p : base.param_names) {
            if (base.array_params.count(p)) sig += 'A';
            else sig += ((mask >> bit++) & 1u) ? 'F' : 'I';
        }
        return sig;
    };
    for (const auto& name : decl_order) {  // natural (all-int) variants first: they get priority under the cap
        if (!bases.count(name)) continue;
        const std::string key = inferer.ensure(name, root_signature(bases.at(name), 0));
        if (!key.empty()) root_keys.insert(key);
    }
    for (const auto& name : decl_order) {
        if (!bases.count(name)) continue;
        const FnInfo& base = bases.at(name);
        size_t scalar_params = 0;
        for (const auto& p : base.param_names) if (!base.array_params.count(p)) scalar_params++;
        if (scalar_params == 0) continue;
        std::vector<unsigned> masks;
        if (scalar_params <= 3) { for (unsigned m = 1; m < (1u << scalar_params); m++) masks.push_back(m); }
        else masks.push_back((1u << scalar_params) - 1);  // all-float
        for (unsigned m : masks) {
            if (insts.size() >= kMaxInstances / 2) break;
            const std::string key = inferer.ensure(name, root_signature(base, m));
            if (!key.empty()) root_keys.insert(key);
        }
    }
    // Outlined loops are called with whatever types the module variables hold; the
    // outliner guessed them from straight-line module code, so request that variant too
    // (the all-int and all-float ones above are the only ones made for 4+ scalars).
    for (const auto& note : mod.loop_notes) {
        if (!note.outlined || note.sig_hint.empty()) continue;
        for (const std::string& fname : note.fns) {
            auto bit = bases.find(fname);
            if (bit == bases.end() || note.sig_hint.size() != bit->second.param_names.size()) continue;
            bool shape_ok = true;
            for (size_t i = 0; i < note.sig_hint.size(); i++)
                if (bit->second.array_params.count(bit->second.param_names[i]) != (note.sig_hint[i] == 'A' ? 1u : 0u)) shape_ok = false;
            if (!shape_ok || insts.size() >= kMaxInstances / 2) continue;
            const std::string key = inferer.ensure(fname, note.sig_hint);
            if (!key.empty()) root_keys.insert(key);
        }
    }
    inferer.run();
    if (inferer.overflowed()) dbg("native variant limit reached; some call chains stay interpreted");

    // ---- Post-inference validation, with a cascade: an instance is only usable
    // if everything it calls is usable too. ----
    for (auto& [key, fn] : insts) {
        std::string why;
        if (!always_returns(fn.decl->body)) why = "does not return on every control path";
        else if (!instance_expressions_ok(insts, fn, why)) {}
        if (!why.empty()) { fn.valid = false; fn.why_invalid = why; }
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& [key, fn] : insts) {
            if (!fn.valid) continue;
            bool all_found = true;
            const auto callees = callee_instances(insts, fn, all_found);
            bool ok = all_found;
            for (const auto& c : callees) if (!insts.at(c).valid) ok = false;
            if (!ok) { fn.valid = false; fn.why_invalid = "calls a variant that cannot be compiled natively"; changed = true; }
        }
    }
    if (debug_on()) {
        for (const auto& key : creation_order) {
            const FnInfo& fn = insts.at(key);
            if (!fn.valid) dbg("'" + fn.base_name + "' variant (" + fn.sig + ") stays interpreted: " + fn.why_invalid);
        }
    }

    // ---- What to emit: valid variants reachable from a valid root ----
    std::unordered_set<std::string> reachable;
    std::vector<std::string> stack;
    for (const auto& key : creation_order)
        if (root_keys.count(key) && insts.at(key).valid) { reachable.insert(key); stack.push_back(key); }
    while (!stack.empty()) {
        const std::string key = stack.back();
        stack.pop_back();
        bool all_found = true;
        for (const auto& callee : callee_instances(insts, insts.at(key), all_found))
            if (reachable.insert(callee).second) stack.push_back(callee);
    }
    std::vector<std::string> emit_order, exported;
    int next_id = 0;
    for (const auto& key : creation_order) {
        if (!reachable.count(key)) continue;
        FnInfo& fn = insts.at(key);
        fn.id = next_id++;
        emit_order.push_back(key);
        if (fn.sig.find('M') == std::string::npos) exported.push_back(key);  // 'M' variants are native-to-native only
    }
    // Per-function outcome for the report.
    std::unordered_map<std::string, std::string> compiled_detail;
    std::unordered_map<std::string, std::unordered_set<std::string>> compiled_global_reads;
    for (const auto& key : emit_order) {
        const FnInfo& fn = insts.at(key);
        if (fn.sig.find('M') != std::string::npos) continue;
        std::string& d = compiled_detail[fn.base_name];
        d += (d.empty() ? "variants: (" : ", (") + fn.sig + ")";
        compiled_global_reads[fn.base_name].insert(fn.global_params.begin(), fn.global_params.end());
    }
    for (const auto& [name, reads] : compiled_global_reads) {
        for (const auto& global : reads)
            compiled_detail[name] += "; reads module variable '" + user_visible_name(global) + "' (passed in)";
    }
    auto variant_reason = [&](const std::string& name) {
        for (const auto& key : creation_order) {
            const FnInfo& fn = insts.at(key);
            if (fn.base_name == name && !fn.valid) return fn.why_invalid + " [variant " + fn.sig + "]";
        }
        return std::string("no usable native variant was produced");
    };
    if (exported.empty()) {
        dbg("no function remained eligible after type inference and validation");
        fill_report({}, variant_reason);
        report.status = "no function remained eligible after type inference";
        return report;
    }

    // ---- Stage 3: C codegen ----
    const std::string c_source = CCodeGen(insts).generate(emit_order, exported);
    report.variants = emit_order.size();
    dbg("compiled " + std::to_string(emit_order.size()) + " native variant(s) of " +
        std::to_string(eligible.size()) + " function(s) to C");


    // CSy's own x86-64 backend: the same typed program, emitted as machine code. It needs no
    // C compiler, so it is the fallback when `cc` is missing, and CSY_NATIVE_BACKEND=x64
    // forces it (used by the differential tests). CSY_NATIVE_BACKEND=c disables it.
    ir.native_x64.clear();
    bool x64_ok = false;
    bool x64_forced = false;
    std::unordered_set<std::string> own_keys;    // instances that run on the own backend although `cc` exists
#ifdef CSY_HAVE_X64_BACKEND
    x64::Image x64_image;
    std::string x64_why;
    {
        const char* be = std::getenv("CSY_NATIVE_BACKEND");
        x64_forced = be && std::string(be) == "x64";
        if (!(be && std::string(be) == "c")) {
            X64CodeGen x64gen(insts);
            x64_ok = x64gen.generate(emit_order, exported, x64_image, x64_why);
            if (x64_ok && !x64_forced) own_keys = own_backend_components(insts, emit_order, x64gen.loop_stats());
            if (x64_ok) {
                ir.native_x64 = x64_serialize(x64_image);
                if (const char* dump = std::getenv("CSY_X64_DUMP")) {   // for inspecting generated code: objdump -D -b binary -mi386:x86-64
                    std::ofstream(dump, std::ios::binary) << x64_image.blob;
                }
            }
            else dbg("own x86-64 backend declined: " + x64_why);
        }
    }
#endif
    // Everything below needs only the generated C. Keep it in the module so an
    // object file can carry it even if no C compiler exists on this machine.
    // Functions with a proven profitable vectorisable loop run on the own backend; the C library then
    // holds only the other components (a library may hold a subset: entries are bound by name and sig).
    const bool all_own = !own_keys.empty() && own_keys.size() == emit_order.size();
    std::string c_source_use = c_source;
    size_t c_variant_count = emit_order.size();
    if (!own_keys.empty()) {
        std::vector<std::string> names;
        for (const auto& key : emit_order) {
            if (!own_keys.count(key)) continue;
            const std::string& base = insts.at(key).base_name;
            if (std::find(names.begin(), names.end(), base) == names.end()) names.push_back(base);
            std::string& d = compiled_detail[base];
            if (d.find("own x86-64 backend") == std::string::npos) d += "; runs on the own x86-64 backend (vectorised element-wise loop)";
        }
        std::string list;
        for (const auto& nme : names) list += (list.empty() ? "" : ", ") + nme;
        dbg("backend choice: own x86-64 backend for " + std::to_string(own_keys.size()) + " variant(s) with proven vectorisable loops (" +
            list + "); " + (all_own ? "none left for C" : std::to_string(emit_order.size() - own_keys.size()) + " stay on C"));
        if (!all_own) {
            std::vector<std::string> order_c, exported_c;
            for (const auto& key : emit_order) if (!own_keys.count(key)) order_c.push_back(key);
            for (const auto& key : exported) if (!own_keys.count(key)) exported_c.push_back(key);
            c_source_use = CCodeGen(insts).generate(order_c, exported_c);
            c_variant_count = order_c.size();
        }
    }
    ir.native_c_source = c_source_use;
    ir.native_target = native_host_target();
    ir.native_library.clear();

    std::string why;
    // Binds (run) or just keeps (build) the machine-code image instead of a C-built library.
    auto use_x64 = [&](const std::string& reason) -> bool {
#ifdef CSY_HAVE_X64_BACKEND
        if (!x64_ok) return false;
        fill_report(compiled_detail, variant_reason);
        if (mode == NativeMode::Run) {
            int loaded = 0;
            std::string load_why;
            if (!x64_load(x64_image, ir, loaded, load_why)) { dbg(load_why); return false; }
            dbg("native fast path active (own x86-64 backend): " + std::to_string(loaded) + " variant(s)");
        }
        report.library_ready = true;
        report.status = std::string(mode == NativeMode::Run ? "native fast path active" : "native code built and embedded") +
                        " (own x86-64 backend" + (reason.empty() ? "" : ": " + reason) + ")";
        return true;
#else
        (void)reason;
        return false;
#endif
    };
    if (x64_forced && use_x64("forced by CSY_NATIVE_BACKEND=x64")) return report;
    if (all_own && use_x64("every native function has a proven vectorisable loop")) return report;
    const std::string so_path = build_library(c_source_use, c_variant_count, why);
    if (so_path.empty()) {
        if (use_x64(why)) return report;
        dbg(why + " - falling back to fully interpreted for all functions in this program");
        std::unordered_map<std::string, std::string> none;
        fill_report(none, [&](const std::string&) { return "compiled to C, but not built: " + why; });
        report.status = why;
        return report;
    }
    report.library_ready = true;
    if (embed_library) {
        std::ifstream in(so_path, std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        if (in && bytes.str().size() <= kMaxEmbeddedLibrary) ir.native_library = bytes.str();
        else dbg("library too large to embed; object will carry the C source only");
    }
    fill_report(compiled_detail, variant_reason);
    if (mode == NativeMode::Run) {
        int loaded = 0;
        if (!bind_library(so_path, ir, loaded, why)) {
            dbg(why);
            report.library_ready = false;
            report.status = why;
            for (auto& f : report.functions) if (f.native) { f.native = false; f.detail = "compiled, but the library could not be loaded: " + why; }
            return report;
        }
        dbg("native fast path active: " + std::to_string(loaded) + " variant(s)");
#ifdef CSY_HAVE_X64_BACKEND
        if (!own_keys.empty()) {
            int extra = 0;
            std::string extra_why;
            if (x64_load(x64_image, ir, extra, extra_why, /*only_missing=*/true))
                dbg("native fast path: " + std::to_string(extra) + " more variant(s) from the own x86-64 backend");
            else dbg("own x86-64 backend could not supply its functions (" + extra_why + "); they run interpreted");
        }
#endif
    }
    report.status = mode == NativeMode::Run ? "native fast path active" : "native code built and embedded";
    if (!own_keys.empty()) report.status += " (own x86-64 backend for functions with vectorisable loops)";
    return report;
}

} // namespace


void outline_top_level_loops(Module& mod) {
    if (std::getenv("CSY_NO_NATIVE_JIT") || std::getenv("CSY_NO_LOOP_OUTLINE")) return;
    LoopOutliner(mod).run();
}

NativeReport try_native_compile(const Module& mod, IRModule& ir, NativeMode mode, bool embed_library) {
    return try_native_compile_impl(mod, ir, mode, embed_library);
}

std::string native_host_target() {
    struct utsname u{};
    if (::uname(&u) != 0) return "unknown";
    return std::string(u.sysname) + "-" + u.machine + "-" + std::to_string(sizeof(void*) * 8);
}

// After a C library was bound, binds the functions it does not hold (the ones the build chose to run on
// the own backend) from the embedded machine-code image. A library that holds every function leaves
// nothing missing, so objects built before this existed behave exactly as before.
static int supplement_from_embedded_x64(IRModule& ir) {
#ifdef CSY_HAVE_X64_BACKEND
    if (ir.native_x64.empty()) return 0;
    x64::Image image;
    if (!x64_parse(ir.native_x64, image)) return 0;
    int extra = 0;
    std::string why;
    if (!x64_load(image, ir, extra, why, /*only_missing=*/true)) { dbg("embedded machine code unusable (" + why + ")"); return 0; }
    if (extra > 0) dbg("native fast path: " + std::to_string(extra) + " more variant(s) from the embedded machine code");
    return extra;
#else
    (void)ir;
    return 0;
#endif
}

size_t attach_embedded_native(IRModule& ir, std::string* note) {
    auto say = [&](const std::string& m) { if (note) *note = m; dbg(m); };
    if (std::getenv("CSY_NO_NATIVE_JIT")) { say("native fast path disabled (CSY_NO_NATIVE_JIT is set)"); return 0; }
    if (ir.native_library.empty() && ir.native_c_source.empty() && ir.native_x64.empty()) { say("object carries no native code"); return 0; }
    std::string why;
    int loaded = 0;
    // 1. The embedded library, if it was built for this kind of machine.
    if (!ir.native_library.empty() && ir.native_target == native_host_target() &&
        ir.native_library.size() >= 4 && std::memcmp(ir.native_library.data(), "\x7f" "ELF", 4) == 0) {
        const std::string dir = cache_dir();
        if (!dir.empty()) {
            const std::string path = dir + "/obj-" + hash_hex(ir.native_library) + ".so";
            struct stat st{};
            const bool present = ::stat(path.c_str(), &st) == 0 && static_cast<size_t>(st.st_size) == ir.native_library.size();
            if (present || write_file_atomic(path, ir.native_library)) {
                if (bind_library(path, ir, loaded, why) && loaded > 0) {
                    loaded += supplement_from_embedded_x64(ir);
                    say("native fast path active from the embedded library: " + std::to_string(loaded) + " variant(s)");
                    return static_cast<size_t>(loaded);
                }
                dbg("embedded library unusable (" + why + "); trying the embedded C source");
                for (auto& f : ir.functions) f.native_variants.clear();
                loaded = 0;
            }
        }
    }
    // 1b. The embedded machine-code image from CSy's own backend (needs no C compiler).
#ifdef CSY_HAVE_X64_BACKEND
    if (!ir.native_x64.empty()) {
        x64::Image image;
        std::string x64_why;
        if (!x64_parse(ir.native_x64, image)) {
            dbg("embedded machine-code image is malformed; ignoring it");
        } else if (x64_load(image, ir, loaded, x64_why) && loaded > 0) {
            say("native fast path active from the embedded machine code: " + std::to_string(loaded) + " variant(s)");
            return static_cast<size_t>(loaded);
        } else {
            dbg("embedded machine code unusable (" + x64_why + ")");
            for (auto& f : ir.functions) f.native_variants.clear();
            loaded = 0;
        }
    }
#endif
    // 2. Rebuild from the embedded C source (cached; needs a C compiler).
    if (!ir.native_c_source.empty()) {
        const std::string so = build_library(ir.native_c_source, 0, why);
        if (!so.empty() && bind_library(so, ir, loaded, why) && loaded > 0) {
            loaded += supplement_from_embedded_x64(ir);
            say("native fast path active from the embedded C source: " + std::to_string(loaded) + " variant(s)");
            return static_cast<size_t>(loaded);
        }
        for (auto& f : ir.functions) f.native_variants.clear();
    }
    say("embedded native code could not be used" + (why.empty() ? std::string() : " (" + why + ")") + "; running interpreted");
    return 0;
}

std::string summarize_native_report(const NativeReport& r) {
    size_t native = 0;
    for (const auto& f : r.functions) if (f.native) native++;
    std::string out = "native fast path: " + std::to_string(native) + " of " + std::to_string(r.functions.size()) +
                      " function(s) compiled";
    if (!r.status.empty() && native != r.functions.size()) out += " (" + r.status + ")";
    if (native != r.functions.size() || r.top_level_loops > 0)
        out += "; run with --explain-native to see why the rest stay interpreted";
    return out;
}

// Project builds mangle top-level names as "__csy_u<N>_<name>"; show the name the user wrote.
static std::string display_name(const std::string& n) {
    if (n.compare(0, 7, "__loop_") == 0) return "top-level loop";
    if (n.compare(0, 7, "__csy_u") == 0) {
        const size_t us = n.find('_', 7);
        if (us != std::string::npos && us + 1 < n.size()) return n.substr(us + 1);
    }
    return n;
}

std::string format_native_report(const NativeReport& r) {
    std::ostringstream o;
    o << "native fast-path report\n";
    size_t native = 0;
    for (const auto& f : r.functions) if (f.native) native++;
    for (const auto& f : r.functions) {
        o << "  " << (f.native ? "native      " : "interpreted ") << display_name(f.name);
        if (f.line > 0) o << " (line " << f.line << ")";
        o << ": " << f.detail << "\n";
    }
    if (r.interpreted_only > 0)
        o << "  interpreted " << r.interpreted_only << " class method(s)/closure(s): never compiled natively\n";
    for (const auto& k : r.kept_loops)
        o << "  interpreted top-level loop (line " << k.line << "): " << k.detail << "\n";
    if (r.top_level_loops > r.kept_loops.size())
        o << "  interpreted " << (r.top_level_loops - r.kept_loops.size())
          << " top-level loop(s): module-level code runs in the interpreter; "
          << "move hot loops into a function to make them eligible\n";
    o << "  " << native << " of " << r.functions.size() << " function(s) native";
    if (!r.status.empty()) o << " - " << r.status;
    o << "\n";
    return o.str();
}

}
