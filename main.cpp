#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <print>
#include <memory>
#include <variant>
#include <vector>

namespace lc::ast {
struct Ast;

struct Var
{
    std::string name;
};

struct Abs
{
    std::string param;
    std::shared_ptr<Ast> body;
};

struct App
{
    std::shared_ptr<Ast> left;
    std::shared_ptr<Ast> right;
};

struct Ast
{
    static inline constexpr auto Idx_var{0uz}, Idx_abs{1uz}, Idx_app{2uz}, Idx_mono{3uz};

    using Node = std::variant<Var, Abs, App, std::monostate>;

    Node node;
};

using Ast_ptr = std::shared_ptr<Ast>;

Ast_ptr make(Ast::Node n)
{
    return std::make_shared<Ast>(Ast{std::move(n)});
}

Ast_ptr var(std::string n)
{
    return make(Var{std::move(n)});
}
Ast_ptr lam(std::string p, Ast_ptr b)
{
    return make(Abs{std::move(p), std::move(b)});
}
Ast_ptr app(Ast_ptr l, Ast_ptr r)
{
    return make(App{std::move(l), std::move(r)});
}

struct Printer {
    void operator()(const Ast &ast) const
    {
        std::visit(*this, ast.node);
    }

    void operator()(const Var &node) const
    {
        std::print("{}", node.name);
    }

    void operator()(const Abs &node) const
    {
        std::print("\\{}.", node.param);
        (*this)(*node.body);
    }

    void operator()(const App &node) const
    {
        // left: only a lambda needs parens, because `\x.x y` would parse as `\x.(x y)`.
        //       A left-nested application needs none, since `f x y` already means `(f x) y`.
        bool paren_l = std::holds_alternative<Abs>(node.left->node);

        // right: a lambda or an application needs parens.
        //        Without them, `f (g x)` would print as `f g x`, i.e. `(f g) x`.
        bool paren_r = !std::holds_alternative<Var>(node.right->node);

        if (paren_l) {
            std::print("(");
        }
        (*this)(*node.left);
        if (paren_l) {
            std::print(")");
        }

        std::print(" ");

        if (paren_r) {
            std::print("(");
        }
        (*this)(*node.right);
        if (paren_r) {
            std::print(")");
        }
    }

    void operator()(const std::monostate &) const
    {
        std::print("<\\0>");
    }
};
};

auto print_ast(const lc::ast::Ast& ast)
{
    lc::ast::Printer{}(ast);
    std::print("\n");
}
// does variable param appear free in this tree? It traverses the tree
bool is_variable_free(const std::string &name, const lc::ast::Ast_ptr &tree_to_search)
{
    using namespace lc::ast;
    return std::visit(
        [&](const auto &search_space) -> bool {
            using T = std::decay_t<decltype(search_space)>;

            if constexpr (std::is_same_v<T, Var>) {
                // Variable if present with the name is free (vars are free).
                return search_space.name == name;
            } else if constexpr (std::is_same_v<T, Abs>) {
                // If parameter is not same as the value, then it is free.
                return search_space.param != name && is_variable_free(name, search_space.body);
            } else if constexpr (std::is_same_v<T, App>) {
                return is_variable_free(name, search_space.left) || is_variable_free(name, search_space.right);
            } else {
                return false;
            }
        },
        tree_to_search->node);
}

struct Substitutor {
    // (\x.y)a (x = name, a = value)
    const std::string &name;      // Thing(name) getting substituted
    const lc::ast::Ast_ptr &value; // Thing(variable term) the name is getting substituted with

    lc::ast::Ast_ptr operator()(const lc::ast::Ast_ptr &ast) const
    {
        return std::visit([&](const auto &n) { return apply(ast, n); }, ast->node);
    }

private:
    lc::ast::Ast_ptr apply(const lc::ast::Ast_ptr &self, const lc::ast::Var &v) const
    {
        return v.name == name ? value : self;
    }

    lc::ast::Ast_ptr apply(const lc::ast::Ast_ptr &self, const lc::ast::Abs &abs) const
    {
        // \x.\x.x === \x.x because the outer x is never used:
        //  |- outer x
        // Return the whole lambda (self; \x.x) unchanged and stop recursing.
        if (abs.param == name) {
            return self;
        }

        // \y.x with x := a  ->  \y.a
        // The binder y does not appear free in `value` (x), so it cannot capture
        // anything. No renaming needed: keep the binder, substitute in the body.
        if (!is_variable_free(abs.param, value)) {
            return make(lc::ast::Abs{abs.param, (*this)(abs.body)});
        }

        // \y.x with x := y  ->  would wrongly give \y.y
        // The binder y IS free in `value`, so rename it first:
        // pick a new binder name (e.g. y') that clashes with nothing
        auto fresh = fresh_name(abs.param, abs.body);
        auto renamed_body = Substitutor{abs.param, make(lc::ast::Var{fresh})}(abs.body);
        // now do the real substitution (name := value) on the renamed body,
        // and wrap the result in a lambda with the new binder: \y'. ...
        return make(lc::ast::Abs{fresh, (*this)(renamed_body)});
    }

    lc::ast::Ast_ptr apply(const lc::ast::Ast_ptr &, const lc::ast::App &app) const
    {
        return make(lc::ast::App{(*this)(app.left), (*this)(app.right)});
    }

    lc::ast::Ast_ptr apply(const lc::ast::Ast_ptr &self, const std::monostate &) const
    {
        return self;
    }

    std::string fresh_name(const std::string &base, const lc::ast::Ast_ptr &body) const
    {
        std::string candidate = base;
        while (candidate == name || is_variable_free(candidate, value) || is_variable_free(candidate, body)) {
            candidate += '\'';
        }
        return candidate;
    }
};

struct Evaluator
{
    lc::ast::Ast_ptr operator()(const lc::ast::Ast_ptr &ast) const
    {
        return std::visit([&](const auto &n) { return eval(ast, n); }, ast->node);
    }

private:
    // already values (or stuck): nothing to do
    lc::ast::Ast_ptr eval(const lc::ast::Ast_ptr &self, const lc::ast::Var &) const
    {
        return self;
    }
    lc::ast::Ast_ptr eval(const lc::ast::Ast_ptr &, const lc::ast::Abs &abs) const
    {
        return make(lc::ast::Abs{abs.param, (*this)(abs.body)});
    }
    lc::ast::Ast_ptr eval(const lc::ast::Ast_ptr &self, const std::monostate &) const
    {
        return self;
    }

    lc::ast::Ast_ptr eval(const lc::ast::Ast_ptr &, const lc::ast::App &app) const
    {
        auto fn = (*this)(app.left);
        auto arg = (*this)(app.right);

        if (auto *abs = std::get_if<lc::ast::Abs>(&fn->node)) {
            // beta reduction
            return (*this)(Substitutor{abs->param, arg}(abs->body));
        }

        // builtin: print
        // Identity + a side effect
        if (auto* v = std::get_if<lc::ast::Var>(&fn->node); v && v->name == "print") {
            lc::ast::Printer{}(*arg);
            std::println();
            return arg;
        }

        // left side isn't a lambda (e.g. free variable): stuck
        return make(lc::ast::App{fn, arg});
    }
};

auto apply(const lc::ast::Ast_ptr & ast)
{
    return Evaluator{}(ast);
}

namespace lc::parse {

enum Token_type { backslash, identifier, dot, paren_l, paren_r, end, print };

struct Pos
{
    std::uint16_t l{}, c{};
};

struct Token
{
    Token_type type;
    std::string text;
    Pos pos;
};

class Lexer
{
public:
    explicit Lexer(std::string_view code) : _code(code)
    {}

    auto tokenize() -> std::vector<Token>
    {
        std::vector<Token> out;

        while (true) {
            skip_whitespace();
            Pos start{line, col}; // position of the token's first char

            if (_index >= _code.size()) {
                out.push_back({Token_type::end, "", start});
                return out;
            }

            char c = _code[_index];
            switch (c) {
            case '\\': {
                advance();
                out.push_back({Token_type::backslash, "\\", start});
            } break;
            case '.': {
                advance();
                out.push_back({Token_type::dot, ".", start});
            } break;
            case '(': {
                advance();
                out.push_back({Token_type::paren_l, "(", start});
            } break;
            case ')': {
                advance();
                out.push_back({Token_type::paren_r, ")", start});
            } break;
            default: {
                if (is_ident_start(c)) {
                    out.push_back(identifier(start));
                } else {
                    throw std::runtime_error(std::format("{}:{}: lex error: unexpected '{}'", line, col, c));
                }
            }
            }
        }
    }

private:
    std::string_view _code;
    std::size_t _index{0};
    std::uint16_t line{1};
    std::uint16_t col{1};

    void advance()
    {
        if (_code[_index] == '\n') {
            ++line; col = 1;
        } else {
            ++col;
        }
        ++_index;
    }

    void skip_whitespace()
    {
        while (_index < _code.size() && std::isspace(static_cast<unsigned char>(_code[_index]))) {
            advance();
        }
    }

    Token identifier(Pos start)
    {
        std::size_t begin = _index;
        while (_index < _code.size() && is_ident_char(_code[_index])) {
            advance();
        }
        auto text = std::string(_code.substr(begin, _index - begin));
        auto type = (text == "print") ? Token_type::print : Token_type::identifier;
        return {type, std::move(text), start};
    }

    static auto is_ident_start(char c) -> bool
    {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    static auto is_ident_char(char c) -> bool
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'';
    }
};

/*
 *  term        -> atom atom*
 *  atom        -> IDENT | '(' term ')' | '\' IDENT '.' term | 'print' term
 */
class Parser
{
public:
    explicit Parser(std::span<const Token> t) : tokens(t)
    {}

    ast::Ast_ptr parse()
    {
        auto result = term();
        expect(Token_type::end, "expected end of input");
        return result;
    }

private:
    std::span<const Token> tokens;
    std::size_t index{0};

    const Token &peek() const
    {
        return tokens[index];
    }

    const Token &advance()
    {
        const Token &t = tokens[index];
        if (t.type != Token_type::end) {
            ++index; // never step past `end`
        }
        return t;
    }

    const Token &expect(Token_type t, std::string_view msg)
    {
        if (peek().type != t) {
            error(msg);
        }
        return advance();
    }

    [[noreturn]] void error(std::string_view msg) const
    {
        const Pos &p = peek().pos;
        throw std::runtime_error(std::format("{}:{}: parse error: {}", p.l, p.c, msg));
    }

    bool starts_atom() const
    {
        auto k = peek().type;
        return k == Token_type::identifier || k == Token_type::paren_l || k == Token_type::backslash || k == Token_type::print;
    }

    // term -> atom atom*
    ast::Ast_ptr term()
    {
        auto left = atom();
        while (starts_atom()) { // loop = left associativity
            auto right = atom();
            left = ast::make(ast::App{left, right});
        }
        return left;
    }

    // atom        -> IDENT | '(' term ')' | '\' IDENT '.' term | 'print' term
    ast::Ast_ptr atom()
    {
        switch (peek().type) {
        case Token_type::identifier: {
            return ast::make(ast::Var{advance().text});
        } break;

        case Token_type::paren_l: {
            advance(); // '('
            auto inner = term();
            expect(Token_type::paren_r, "expected ')'");
            return inner;
        } break;

        case Token_type::backslash: {
            advance(); // '\'
            std::string param = expect(Token_type::identifier, "expected parameter name").text;
            expect(Token_type::dot, "expected '.'");
            auto body = term();
            return ast::make(ast::Abs{std::move(param), body});
        } break;

        case Token_type::print: {
            advance();
            auto operand = term();
            return ast::make(ast::App{ast::make(ast::Var{"print"}), operand});
        } break;

        default:
            error("expected a term");
        }
    }
};
}

auto print_usage(char * prog)
{
    std::println("Usage: ");
    std::println("  {} <file>", prog);
}

auto main(int argc, char *argv[]) -> int
{
    if (argc < 1) {
        std::println(stderr, "Error: no file");
        print_usage(argv[0]);
    }

    try {
        if (!std::filesystem::exists(argv[1])) {
            throw std::invalid_argument("File doesn't exist");
        }

        std::fstream file(argv[1]);
        std::string code{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        print_ast(*apply(lc::parse::Parser(lc::parse::Lexer(code).tokenize()).parse()));
    } catch (std::exception *e) {
        std::println("Error: {}", e->what());
        return EXIT_FAILURE;
    }
    return 0;
}
