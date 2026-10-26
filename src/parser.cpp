#include "parser.hpp"
#include "suggest.hpp"
#include <cctype>
#include <unordered_map>
#include <stdexcept>
#include <sstream>
#include <cmath>
#include "resource.hpp"

namespace csy {

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const std::string& file,
           const std::string& source)
        : tokens_(tokens), file_(file), source_(source) { scan_aliases(); }

    std::vector<StmtPtr> pending_extra_;

    ParseResult run() {
        ParseResult r;
        r.module.file = file_;
        try {
            while (!at_end()) {
                if (check(TokenType::END_OF_FILE)) break;
                auto s = parse_stmt();
                if (s) r.module.body.push_back(s);
                for (auto& extra : pending_extra_) r.module.body.push_back(extra);
                pending_extra_.clear();
            }
            r.module.explains = std::move(explains_);
            if (!directive_aliases_.empty()) check_alias_collisions(r.module);
            r.ok = !diags_.has_errors();
        } catch (const std::runtime_error& e) {
            diags_.error(file_, cur_line(), cur_col(), e.what());
            r.ok = false;
        }
        r.diags = std::move(diags_);
        return r;
    }

private:
    struct DepthScope {
        size_t& depth;
        explicit DepthScope(size_t& current, const char* what) : depth(current) {
            if (depth >= 256) throw std::runtime_error(std::string("maximum ") + what + " nesting depth exceeded (256)");
            ++depth;
        }
        ~DepthScope() { --depth; }
    };

    const std::vector<Token>& tokens_;
    const std::string& file_;
    const std::string& source_;
    size_t pos_ = 0;
    size_t stmt_counter_ = 0;
    size_t statement_depth_ = 0;

    struct AliasInfo { TokenType type; size_t decl_pos; int line; int col; std::string directive; };
    std::unordered_map<std::string, AliasInfo> directive_aliases_;
    std::vector<StmtPtr> explains_;
    static constexpr size_t kMaxExplainText = 4096;
    static constexpr size_t kMaxExplainRefs = 256;

    void scan_aliases() {
        for (size_t i = 0; i + 3 < tokens_.size(); ++i) {
            if (tokens_[i].type != TokenType::KW_USING) continue;
            const TokenType d = tokens_[i + 1].type;
            if (d != TokenType::KW_INC && d != TokenType::KW_CONNECT && d != TokenType::KW_EXPLAIN) continue;
            if (tokens_[i + 2].type != TokenType::KW_AS || tokens_[i + 3].type != TokenType::IDENT) continue;
            directive_aliases_.emplace(tokens_[i + 3].text,
                                       AliasInfo{d, i, tokens_[i + 3].line, tokens_[i + 3].col, tokens_[i + 1].text});
        }
    }

    void skip_statement() {
        while (!at_end() && !check(TokenType::SEMI) && !check(TokenType::END_OF_FILE)) advance();
        if (check(TokenType::SEMI)) advance();
    }

    void check_alias_collisions(const Module& module) {
        std::unordered_map<std::string, std::string> used;
        for (const auto& s : module.body) {
            if (!s) continue;
            switch (s->kind) {
                case StmtKind::FuncDecl:
                    if (!s->func_name.empty() && s->func_name[0] != '$') used.emplace(s->func_name, "the function '" + s->func_name + "'");
                    break;
                case StmtKind::VarDecl:
                    if (s->expr && s->expr->kind == ExprKind::Assign && s->expr->target && s->expr->target->kind == ExprKind::Ident)
                        used.emplace(s->expr->target->name, "the variable '" + s->expr->target->name + "'");
                    break;
                case StmtKind::Import: used.emplace(s->import_alias, "the #inc name '" + s->import_alias + "'"); break;
                case StmtKind::Connect: used.emplace(s->connect_alias, "the #connect namespace '" + s->connect_alias + "'"); break;
                case StmtKind::Using: used.emplace(s->using_alias, "the using name '" + s->using_alias + "'"); break;
                default: break;
            }
        }
        for (const auto& kv : directive_aliases_) {
            auto clash = used.find(kv.first);
            if (clash != used.end())
                diags_.error(file_, kv.second.line, kv.second.col,
                             "the alias '" + kv.first + "' for " + kv.second.directive + " is already used as " + clash->second + " in this file");
        }
    }
    size_t expression_depth_ = 0;
    size_t unary_depth_ = 0;
    DiagBag diags_;

    const Token& peek(int ahead = 0) const {
        static Token sentinel;
        size_t p = pos_ + ahead;
        if (p >= tokens_.size()) return sentinel;
        return tokens_[p];
    }
    const Token& current() const { return peek(0); }
    bool at_end() const { return pos_ >= tokens_.size() || current().type == TokenType::END_OF_FILE; }
    bool check(TokenType t) const { return current().type == t; }
    bool check_any(std::initializer_list<TokenType> ts) const {
        for (auto t : ts) if (check(t)) return true;
        return false;
    }
    const Token& advance() {
        const Token& t = peek(0);
        if (!at_end()) pos_++;
        return t;
    }
    const Token& expect(TokenType t, const std::string& what) {
        if (check(t)) return advance();
        if (t == TokenType::SEMI) {
            warn(current(), "missing ';' - the compiler is treating one as inserted here");
            return current();
        }
        error(current(), "expected " + what + " but got '" + current().text + "'");
        return current();
    }
    int cur_line() const { return current().line; }
    int cur_col() const { return current().col; }

    void error(const Token& tok, const std::string& msg) {
        diags_.error(file_, tok.line, tok.col, msg);
        if (diags_.error_limit_reached()) {
            throw std::runtime_error("too many errors in this file - stopping here");
        }
    }

    ExprPtr seal(ExprPtr e) {
        uint32_t child = 0;
        auto take = [&](const ExprPtr& c) { if (c && c->depth > child) child = c->depth; };
        take(e->left); take(e->right); take(e->operand); take(e->target); take(e->value);
        take(e->callee); take(e->object); take(e->index);
        for (const auto& a : e->args) take(a);
        e->depth = child + 1;
        if (e->depth > kMaxExprTreeDepth) {
            throw std::runtime_error("expression is too long or too deeply chained (limit " +
                                     std::to_string(kMaxExprTreeDepth) +
                                     " levels) - split it into several statements");
        }
        return e;
    }
    void warn(const Token& tok, const std::string& msg) {
        diags_.warn(file_, tok.line, tok.col, msg);
    }

    StmtPtr parse_stmt() {
        DepthScope depth(statement_depth_, "statement");
        if ((++stmt_counter_ & 0x3ff) == 0) res::check("parsing");
        const Token& t = current();
        switch (t.type) {
            case TokenType::KW_INC:   return parse_import();
            case TokenType::KW_CONNECT: return parse_connect();
            case TokenType::KW_EXPLAIN: return parse_explain();
            case TokenType::KW_FUNC:  return parse_func();
            case TokenType::KW_IF:    return parse_if();
            case TokenType::KW_WHILE: return parse_while();
            case TokenType::KW_FOR:   return parse_for();
            case TokenType::KW_RT:    return parse_return();
            case TokenType::KW_USING: return parse_using();
            case TokenType::KW_CLASS: return parse_class();
            case TokenType::KW_TRY:   return parse_try();
            case TokenType::KW_STOP: {
                Token t = advance();
                expect(TokenType::SEMI, "';'");
                return make_stmt(StmtKind::Break, t);
            }
            case TokenType::KW_SKIP: {
                Token t = advance();
                expect(TokenType::SEMI, "';'");
                return make_stmt(StmtKind::Continue, t);
            }
            case TokenType::EXT:
                error(t, "unexpected '::ext' - no matching block to close");
                advance();
                return nullptr;
            default:
                if (t.type == TokenType::IDENT && statement_depth_ == 1) {
                    auto alias = directive_aliases_.find(t.text);
                    if (alias != directive_aliases_.end()) {
                        if (pos_ < alias->second.decl_pos) {
                            error(t, "'" + t.text + "' is used before it is declared as an alias for " + alias->second.directive +
                                     " (the 'using " + alias->second.directive + " as " + t.text + ";' is on line " +
                                     std::to_string(alias->second.line) + ")");
                            skip_statement();
                            return nullptr;
                        }
                        switch (alias->second.type) {
                            case TokenType::KW_INC: return parse_import();
                            case TokenType::KW_CONNECT: return parse_connect();
                            default: return parse_explain();
                        }
                    }
                }
                if (t.type == TokenType::IDENT && t.text == "elif") {
                    error(t, "'elif' isn't CSy - this language uses 'orf' instead");
                    advance();
                    if (!check(TokenType::COLON)) {
                        parse_expr();
                    }
                    if (check(TokenType::COLON)) advance();
                    parse_block_body();
                    return nullptr;
                }
                if (t.type == TokenType::IDENT && t.text == "inc") {
                    warn(t, "missing '#' before 'inc' - treating this as '#inc'");
                    return parse_import();
                }
                return parse_expr_or_vardecl();
        }
    }

    StmtPtr parse_import() {
        Token inc = advance();  // #inc
        if (!check(TokenType::IDENT)) {
            error(current(), "expected module name after '#inc'");
            return nullptr;
        }
        Token mod = advance();
        std::string module_name = mod.text;
        std::string alias = module_name;  // default alias = module name
        if (check(TokenType::KW_AS)) {
            advance();
            if (!check(TokenType::IDENT)) {
                error(current(), "expected alias name after 'as'");
                return nullptr;
            }
            Token al = advance();
            alias = al.text;
        }
        expect(TokenType::SEMI, "';'");
        auto s = make_stmt(StmtKind::Import, inc);
        s->import_module = module_name;
        s->import_alias = alias;
        return s;
    }

    static std::string default_namespace(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name.erase(dot);
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])) || is_keyword(name)) return std::string();
        for (unsigned char c : name) if (!(std::isalnum(c) || c == '_')) return std::string();
        return name;
    }

    StmtPtr parse_connect() {
        Token directive = advance();
        if (!check(TokenType::STRING)) {
            error(current(), "expected a quoted .csy path after '#connect'");
            return nullptr;
        }
        Token path = advance();
        std::string alias = default_namespace(path.text);
        if (check(TokenType::KW_AS)) {
            advance();
            if (!check(TokenType::IDENT)) {
                error(current(), "expected a namespace name after 'as'");
                return nullptr;
            }
            alias = advance().text;
        } else if (alias.empty()) {
            error(path, "the file name in '" + path.text + "' cannot be used as a namespace name; write 'as name' after the path");
            return nullptr;
        }
        auto stmt = make_stmt(StmtKind::Connect, directive);
        stmt->connect_path = path.text;
        stmt->connect_alias = alias;
        if (!check(TokenType::LBRACE)) {
            stmt->connect_all = true;
            expect(TokenType::SEMI, "';'");
            return stmt;
        }
        advance();
        while (!at_end() && !check(TokenType::RBRACE)) {
            if (!check(TokenType::IDENT)) {
                error(current(), "expected a function or variable name in the '#connect' allowlist");
                advance();
                continue;
            }
            stmt->connect_exports.push_back(advance().text);
            if (check(TokenType::SEMI) || check(TokenType::COMMA)) advance();
            else if (!check(TokenType::RBRACE)) {
                error(current(), "expected ';' or ',' between connected names");
                break;
            }
        }
        if (check(TokenType::RBRACE)) advance();
        else error(current(), "expected '}' to close the '#connect' allowlist");
        if (check(TokenType::SEMI)) advance();
        return stmt;
    }

    StmtPtr parse_directive_alias(const Token& using_tok, size_t using_pos) {
        Token directive = advance();
        if (statement_depth_ != 1) {
            error(using_tok, "a directive alias can only be declared at the top level of a file");
            skip_statement();
            return nullptr;
        }
        if (!check(TokenType::KW_AS)) {
            error(current(), "expected 'as' and the new name after '" + directive.text + "'");
            skip_statement();
            return nullptr;
        }
        advance();
        if (!check(TokenType::IDENT)) {
            error(current(), "expected the new name for '" + directive.text + "' after 'as'");
            skip_statement();
            return nullptr;
        }
        Token alias = advance();
        expect(TokenType::SEMI, "';'");
        auto known = directive_aliases_.find(alias.text);
        if (known != directive_aliases_.end() && known->second.decl_pos != using_pos) {
            error(alias, "'" + alias.text + "' is already an alias for " + known->second.directive + " (declared on line " +
                         std::to_string(known->second.line) + ")");
            return nullptr;
        }
        auto s = make_stmt(StmtKind::DirectiveAlias, using_tok);
        s->alias_directive = directive.text;
        s->using_alias = alias.text;
        return s;
    }

    bool read_qualified_name(std::string& out) {
        if (!check(TokenType::IDENT)) return false;
        out = advance().text;
        if (check(TokenType::DCOLON) && pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].type == TokenType::IDENT) {
            advance();
            out += "::" + advance().text;
        }
        return true;
    }

    StmtPtr parse_explain() {
        Token directive = advance();
        auto fail = [&](const std::string& msg) -> StmtPtr { error(current(), msg); skip_statement(); return nullptr; };
        std::string name;
        if (!read_qualified_name(name))
            return fail("expected the function or variable name to explain after '#explain' (to write a plain comment that starts "
                        "with the word explain, put a space after the '#')");
        if (!check(TokenType::GT)) return fail("expected '>' and the explanation in quotes after '#explain " + name + "'");
        advance();
        if (!check(TokenType::STRING)) return fail("expected the explanation in quotes after '>'");
        Token text = advance();
        if (text.text.size() > kMaxExplainText)
            return fail("the explanation is longer than " + std::to_string(kMaxExplainText) + " characters");
        std::vector<std::string> refs;
        if (check(TokenType::IDENT) && current().text == "ref") {
            advance();
            if (!check(TokenType::LPAREN)) return fail("expected '(' after 'ref'");
            advance();
            while (!check(TokenType::RPAREN)) {
                std::string ref;
                if (!read_qualified_name(ref)) return fail("expected a function or variable name inside ref(...)");
                if (refs.size() >= kMaxExplainRefs) return fail("ref(...) lists more than " + std::to_string(kMaxExplainRefs) + " names");
                refs.push_back(ref);
                if (check(TokenType::COMMA)) advance();
                else if (!check(TokenType::RPAREN)) return fail("expected ',' or ')' in ref(...)");
            }
            advance();
        }
        expect(TokenType::SEMI, "';'");
        auto s = make_stmt(StmtKind::Explain, directive);
        s->explain_name = name;
        s->explain_text = text.text;
        s->explain_refs = std::move(refs);
        explains_.push_back(s);
        return nullptr;
    }

    StmtPtr parse_using() {
        const size_t using_pos = pos_;
        Token u = advance();
        if (check(TokenType::KW_INC) || check(TokenType::KW_CONNECT) || check(TokenType::KW_EXPLAIN))
            return parse_directive_alias(u, using_pos);
        // using <expr> as <name>;
        ExprPtr e = parse_expr();
        if (!check(TokenType::KW_AS)) {
            error(current(), "expected 'as' after using-expression");
            return nullptr;
        }
        advance();
        if (!check(TokenType::IDENT)) {
            error(current(), "expected alias name after 'as'");
            return nullptr;
        }
        Token al = advance();
        expect(TokenType::SEMI, "';'");
        auto s = make_stmt(StmtKind::Using, u);
        s->using_expr = e;
        s->using_alias = al.text;
        return s;
    }

    StmtPtr parse_func() {
        Token f = advance();  // func
        if (!check(TokenType::IDENT)) {
            error(current(), "expected function name after 'func'");
            return nullptr;
        }
        Token name = advance();
        expect(TokenType::LPAREN, "'('");
        std::vector<std::string> params;
        while (!check(TokenType::RPAREN)) {
            if (!check(TokenType::IDENT)) {
                error(current(), "expected parameter name");
                return nullptr;
            }
            Token p = advance();
            params.push_back(p.text);
            if (check(TokenType::COMMA)) {
                advance();
                if (check(TokenType::RPAREN)) break;
            } else {
                break;
            }
        }
        expect(TokenType::RPAREN, "')'");
        expect(TokenType::COLON, "':'");
        std::vector<StmtPtr> body = parse_block_body();
        expect(TokenType::EXT, "'::ext'");
        auto s = make_stmt(StmtKind::FuncDecl, f);
        s->func_name = name.text;
        s->params = std::move(params);
        s->body = std::move(body);
        return s;
    }

    StmtPtr parse_class() {
        Token c = advance();
        if (!check(TokenType::IDENT)) {
            error(current(), "expected class name after 'class'");
            return nullptr;
        }
        Token name = advance();
        std::string class_name = name.text;

        expect(TokenType::LPAREN, "'('");
        std::vector<std::string> fields;
        while (!check(TokenType::RPAREN)) {
            if (!check(TokenType::IDENT)) {
                error(current(), "expected a field name");
                return nullptr;
            }
            Token fld = advance();
            fields.push_back(fld.text);
            if (check(TokenType::COMMA)) {
                advance();
                if (check(TokenType::RPAREN)) break;
            } else {
                break;
            }
        }
        expect(TokenType::RPAREN, "')'");

        bool has_base = false;
        std::string base_name;
        std::vector<ExprPtr> base_args;
        if (check(TokenType::KW_EXTENDS)) {
            advance();
            if (!check(TokenType::IDENT)) {
                error(current(), "expected base class name after 'extends'");
                return nullptr;
            }
            has_base = true;
            base_name = advance().text;
            expect(TokenType::LPAREN, "'(' after base class name");
            while (!check(TokenType::RPAREN)) {
                base_args.push_back(parse_expr());
                if (check(TokenType::COMMA)) {
                    advance();
                    if (check(TokenType::RPAREN)) break;
                } else {
                    break;
                }
            }
            expect(TokenType::RPAREN, "')'");
        }
        expect(TokenType::COLON, "':'");

        std::vector<std::pair<std::string, std::string>> methods;
        while (!check(TokenType::EXT) && !at_end()) {
            if (!check(TokenType::KW_FUNC)) {
                error(current(), "expected a method ('func ...') inside class '" + class_name + "'");
                advance();
                continue;
            }
            StmtPtr method = parse_func();
            if (!method) continue;
            std::string bare_name = method->func_name;
            std::string mangled = "$" + class_name + "$" + bare_name;
            method->func_name = mangled;
            methods.push_back({bare_name, mangled});
            pending_extra_.push_back(method);
        }
        expect(TokenType::EXT, "'::ext'");

        auto ctor = make_stmt(StmtKind::FuncDecl, c);
        ctor->func_name = class_name;
        ctor->params = fields;

        auto dict_lit = make_expr(ExprKind::DictLiteral, c);
        for (const auto& f : fields) {
            auto key = make_expr(ExprKind::String, c);
            key->str_val = f;
            auto val = make_expr(ExprKind::Ident, c);
            val->name = f;
            dict_lit->dict_pairs.push_back({key, val});
        }
        for (const auto& m : methods) {
            auto key = make_expr(ExprKind::String, c);
            key->str_val = m.first;
            auto val = make_expr(ExprKind::Ident, c);
            val->name = m.second;
            dict_lit->dict_pairs.push_back({key, val});
        }

        ExprPtr ret_value = dict_lit;
        if (has_base) {
            std::string base_tmp = "$base$" + class_name;
            auto base_call = make_expr(ExprKind::Call, c);
            auto base_callee = make_expr(ExprKind::Ident, c);
            base_callee->name = base_name;
            base_call->callee = base_callee;
            base_call->args = base_args;

            auto base_assign = make_expr(ExprKind::Assign, c);
            auto base_target = make_expr(ExprKind::Ident, c);
            base_target->name = base_tmp;
            base_assign->target = base_target;
            base_assign->value = base_call;
            auto base_stmt = make_stmt(StmtKind::VarDecl, c);
            base_stmt->expr = base_assign;
            ctor->body.push_back(base_stmt);

            auto merge_call = make_expr(ExprKind::Call, c);
            auto merge_callee = make_expr(ExprKind::Ident, c);
            merge_callee->name = "dict_merge";
            merge_call->callee = merge_callee;
            auto base_ref = make_expr(ExprKind::Ident, c);
            base_ref->name = base_tmp;
            merge_call->args = {base_ref, dict_lit};
            ret_value = merge_call;
        }

        auto ret = make_stmt(StmtKind::Return, c);
        ret->ret_value = ret_value;
        ctor->body.push_back(ret);

        return ctor;
    }

    StmtPtr parse_try() {
        Token t = advance();
        expect(TokenType::COLON, "':'");
        std::vector<StmtPtr> try_body = parse_block_body();
        if (!check(TokenType::KW_CATCH)) {
            error(current(), "expected 'catch' after 'try' block");
            if (check(TokenType::EXT)) advance();
            return nullptr;
        }
        advance();  // catch
        if (!check(TokenType::IDENT)) {
            error(current(), "expected a name to bind the error to after 'catch'");
            return nullptr;
        }
        std::string catch_var = advance().text;
        expect(TokenType::COLON, "':'");
        std::vector<StmtPtr> catch_body = parse_block_body();
        expect(TokenType::EXT, "'::ext'");
        auto s = make_stmt(StmtKind::Try, t);
        s->body = std::move(try_body);
        s->catch_var = catch_var;
        s->then_body = std::move(catch_body);
        return s;
    }

    StmtPtr parse_if() {
        Token i = advance();
        ExprPtr cond = parse_expr();
        expect(TokenType::COLON, "':'");
        std::vector<StmtPtr> then_body = parse_block_body();
        std::vector<std::pair<ExprPtr, std::vector<StmtPtr>>> elifs;
        std::vector<StmtPtr> else_body;
        while (check(TokenType::KW_ELIF) || check(TokenType::KW_ELSE)) {
            if (check(TokenType::KW_ELIF)) {
                advance();
                ExprPtr econd = parse_expr();
                expect(TokenType::COLON, "':'");
                auto ebody = parse_block_body();
                elifs.emplace_back(econd, std::move(ebody));
            } else {
                advance();
                expect(TokenType::COLON, "':'");
                else_body = parse_block_body();
                break;
            }
        }
        expect(TokenType::EXT, "'::ext'");
        auto s = make_stmt(StmtKind::If, i);
        s->cond = cond;
        s->then_body = std::move(then_body);
        s->elifs = std::move(elifs);
        s->else_body = std::move(else_body);
        return s;
    }

    StmtPtr parse_while() {
        Token w = advance();
        ExprPtr cond = parse_expr();
        expect(TokenType::COLON, "':'");
        auto body = parse_block_body();
        expect(TokenType::EXT, "'::ext'");
        auto s = make_stmt(StmtKind::While, w);
        s->cond = cond;
        s->body = std::move(body);
        return s;
    }

    StmtPtr parse_for() {
        Token f = advance();
        if (!check(TokenType::IDENT)) {
            error(current(), "expected loop variable name after 'for'");
            return nullptr;
        }
        Token lv = advance();
        if (!check(TokenType::KW_IN)) {
            error(current(), "expected 'in' after loop variable");
            return nullptr;
        }
        advance();
        ExprPtr iter;
        if (check(TokenType::KW_THRU)) {
            Token thru_tok = advance();
            ExprPtr bound = parse_expr();
            auto callee = make_expr(ExprKind::Ident, thru_tok);
            callee->name = "range";
            auto call = make_expr(ExprKind::Call, thru_tok);
            call->callee = callee;
            call->args.push_back(bound);
            iter = call;
        } else {
            iter = parse_expr();
        }
        expect(TokenType::COLON, "':'");
        auto body = parse_block_body();
        expect(TokenType::EXT, "'::ext'");
        auto s = make_stmt(StmtKind::For, f);
        s->loop_var = lv.text;
        s->iterable = iter;
        s->body = std::move(body);
        return s;
    }

    StmtPtr parse_return() {
        Token r = advance();
        ExprPtr value = nullptr;
        if (!check(TokenType::SEMI)) {
            value = parse_expr();
        }
        expect(TokenType::SEMI, "';'");
        auto s = make_stmt(StmtKind::Return, r);
        s->ret_value = value;
        return s;
    }

    StmtPtr parse_expr_or_vardecl() {
        Token start = current();
        ExprPtr e = parse_expr();
        if (check(TokenType::ASSIGN)) {
            Token eq = advance();
            ExprPtr value = parse_expr();
            expect(TokenType::SEMI, "';'");
            auto s = make_stmt(StmtKind::VarDecl, start);
            auto ae = make_expr(ExprKind::Assign, eq);
            ae->target = e;
            ae->value = value;
            s->expr = ae;
            return s;
        }
        if (check(TokenType::PLUS_EQ) || check(TokenType::MINUS_EQ)) {
            Token op = advance();
            ExprPtr value = parse_expr();
            expect(TokenType::SEMI, "';'");
            auto s = make_stmt(StmtKind::ExprStmt, start);
            auto ae = make_expr(ExprKind::CompoundAssign, op);
            ae->op = op.type;
            ae->target = e;
            ae->value = value;
            s->expr = ae;
            return s;
        }
        expect(TokenType::SEMI, "';'");
        auto s = make_stmt(StmtKind::ExprStmt, start);
        s->expr = e;
        return s;
    }

    std::vector<StmtPtr> parse_block_body() {
        std::vector<StmtPtr> body;
        while (!at_end()) {
            if (check(TokenType::EXT)) break;
            if (check(TokenType::KW_ELIF) || check(TokenType::KW_ELSE)) break;
            if (check(TokenType::KW_CATCH)) break;
            auto s = parse_stmt();
            if (s) body.push_back(s);
        }
        return body;
    }


    ExprPtr parse_expr() {
        DepthScope depth(expression_depth_, "expression");
        return parse_or();
    }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (check(TokenType::OR)) {
            Token op = advance();
            ExprPtr right = parse_and();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_equality();
        while (check(TokenType::AND)) {
            Token op = advance();
            ExprPtr right = parse_equality();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_equality() {
        ExprPtr left = parse_comparison();
        while (check(TokenType::EQ) || check(TokenType::NEQ)) {
            Token op = advance();
            ExprPtr right = parse_comparison();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_comparison() {
        ExprPtr left = parse_additive();
        while (check_any({TokenType::LT, TokenType::GT, TokenType::LE, TokenType::GE})) {
            Token op = advance();
            ExprPtr right = parse_additive();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_additive() {
        ExprPtr left = parse_multiplicative();
        while (check(TokenType::PLUS) || check(TokenType::MINUS)) {
            Token op = advance();
            ExprPtr right = parse_multiplicative();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_multiplicative() {
        ExprPtr left = parse_unary();
        while (check_any({TokenType::STAR, TokenType::SLASH, TokenType::PERCENT})) {
            Token op = advance();
            ExprPtr right = parse_unary();
            auto e = make_expr(ExprKind::Binary, op);
            e->op = op.type;
            e->left = left;
            e->right = right;
            left = seal(e);
        }
        return left;
    }

    ExprPtr parse_unary() {
        DepthScope depth(unary_depth_, "unary expression");
        if (check(TokenType::NOT) || check(TokenType::MINUS)) {
            Token op = advance();
            ExprPtr operand = parse_unary();
            auto e = make_expr(ExprKind::Unary, op);
            e->op = op.type;
            e->operand = operand;
            return seal(e);
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        ExprPtr e = parse_primary();
        while (true) {
            if (check(TokenType::LPAREN)) {
                Token lp = advance();
                std::vector<ExprPtr> args;
                while (!check(TokenType::RPAREN)) {
                    args.push_back(parse_expr());
                    if (check(TokenType::COMMA)) {
                        advance();
                        if (check(TokenType::RPAREN)) break;  // trailing comma
                    } else {
                        break;
                    }
                }
                expect(TokenType::RPAREN, "')'");
                auto ce = make_expr(ExprKind::Call, lp);
                ce->callee = e;
                ce->args = std::move(args);
                e = seal(ce);
            } else if (check(TokenType::DCOLON) || check(TokenType::DOT)) {
                bool legacy_dot = check(TokenType::DOT);
                Token accessor = advance();
                if (legacy_dot) {
                    warn(accessor, "'.' is the old CSy member access - use '::' instead (e.g. bc::out, not bc.out)");
                }
                if (!check(TokenType::IDENT)) {
                    error(current(), "expected member name after '::'");
                    break;
                }
                Token name = advance();
                auto me = make_expr(ExprKind::Member, name);
                me->object = e;
                me->name = name.text;
                e = seal(me);
            } else if (check(TokenType::LBRACKET)) {
                Token bracket = advance();
                ExprPtr idx = parse_expr();
                expect(TokenType::RBRACKET, "']'");
                auto ie = make_expr(ExprKind::Index, bracket);
                ie->object = e;
                ie->index = idx;
                e = seal(ie);
            } else {
                break;
            }
        }
        return e;
    }

    ExprPtr parse_primary() {
        const Token& t = current();
        switch (t.type) {
            case TokenType::INT: {
                advance();
                auto e = make_expr(ExprKind::Int, t);
                try { e->int_val = std::stoll(t.text); }
                catch (...) {
                    error(t, "integer literal '" + (t.text.size() > 40 ? t.text.substr(0, 40) + "..." : t.text) +
                             "' does not fit in a 64-bit int (range -9223372036854775807..9223372036854775807)");
                    diags_.note("write it as a float (e.g. 1e19) if an approximate value is acceptable");
                }
                return e;
            }
            case TokenType::FLOAT: {
                advance();
                auto e = make_expr(ExprKind::Float, t);
                try { e->float_val = std::stod(t.text); }
                catch (...) {
                    error(t, "number literal '" + (t.text.size() > 40 ? t.text.substr(0, 40) + "..." : t.text) +
                             "' is outside the range of a 64-bit float");
                }
                return e;
            }
            case TokenType::STRING: {
                advance();
                auto e = make_expr(ExprKind::String, t);
                e->str_val = t.text;
                return e;
            }
            case TokenType::KW_TRUE: {
                advance();
                auto e = make_expr(ExprKind::Bool, t);
                e->bool_val = true;
                return e;
            }
            case TokenType::KW_FALSE: {
                advance();
                auto e = make_expr(ExprKind::Bool, t);
                e->bool_val = false;
                return e;
            }
            case TokenType::KW_NONE: {
                advance();
                return make_expr(ExprKind::None, t);
            }
            case TokenType::IDENT: {
                advance();
                auto e = make_expr(ExprKind::Ident, t);
                e->name = t.text;
                return e;
            }
            case TokenType::LPAREN: {
                advance();
                ExprPtr e = parse_expr();
                expect(TokenType::RPAREN, "')'");
                return e;
            }
            case TokenType::LBRACKET: {
                advance();
                std::vector<ExprPtr> elements;
                while (!check(TokenType::RBRACKET)) {
                    elements.push_back(parse_expr());
                    if (check(TokenType::COMMA)) {
                        advance();
                        if (check(TokenType::RBRACKET)) break;
                    } else {
                        break;
                    }
                }
                expect(TokenType::RBRACKET, "']'");
                auto e = make_expr(ExprKind::VectorLiteral, t);
                e->elements = std::move(elements);
                return e;
            }
            case TokenType::LBRACE: {
                advance();
                std::vector<std::pair<ExprPtr, ExprPtr>> pairs;
                while (!check(TokenType::RBRACE)) {
                    ExprPtr key = parse_expr();
                    expect(TokenType::COLON, "':'");
                    ExprPtr val = parse_expr();
                    pairs.push_back({key, val});
                    if (check(TokenType::COMMA)) {
                        advance();
                        if (check(TokenType::RBRACE)) break;
                    } else {
                        break;
                    }
                }
                expect(TokenType::RBRACE, "'}'");
                auto e = make_expr(ExprKind::DictLiteral, t);
                e->dict_pairs = std::move(pairs);
                return e;
            }
            default:
                error(t, "expected an expression but got '" + t.text + "'");
                advance();  // skip the bad token to make progress
                return make_expr(ExprKind::None, t);
        }
    }
};

ParseResult parse(const std::vector<Token>& tokens,
                  const std::string& file,
                  const std::string& source) {
    Parser p(tokens, file, source);
    return p.run();
}

}
