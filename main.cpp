#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory_resource>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace lc::ast {

using NodeIdx = std::uint32_t;

struct Var
{
    std::string name;
};

struct Abs
{
    std::string param;
    NodeIdx body;
};

struct App
{
    NodeIdx left;
    NodeIdx right;
};

using NodeData = std::variant<Var, Abs, App, std::monostate>;

struct AstContext
{
    std::pmr::vector<NodeData> memory;

    explicit AstContext(std::pmr::memory_resource *mr) : memory(mr)
    {
        memory.reserve(100000); // Prevent frequent vector reallocations
    }

    auto make_var(std::string name) -> NodeIdx
    {
        memory.push_back(Var{std::move(name)});
        return memory.size() - 1;
    }

    auto make_abs(std::string param, NodeIdx body) -> NodeIdx
    {
        memory.push_back(Abs{std::move(param), body});
        return memory.size() - 1;
    }

    auto make_app(NodeIdx left, NodeIdx right) -> NodeIdx
    {
        memory.push_back(App{left, right});
        return memory.size() - 1;
    }

    auto make_mono() -> NodeIdx
    {
        memory.push_back(std::monostate{});
        return memory.size() - 1;
    }

    auto get(NodeIdx idx) -> NodeData &
    {
        return memory[idx];
    }
    auto get(NodeIdx idx) const -> const NodeData &
    {
        return memory[idx];
    }
};

struct Printer
{
    AstContext &ctx;

    auto operator()(NodeIdx ast) const -> void
    {
        std::visit(*this, ctx.get(ast));
    }

    auto operator()(const Var &node) const -> void
    {
        std::print("{}", node.name);
    }

    auto operator()(const Abs &node) const -> void
    {
        std::print("\\{}.", node.param);
        (*this)(node.body);
    }

    auto operator()(const App &node) const -> void
    {
        bool paren_l = std::holds_alternative<Abs>(ctx.get(node.left));
        bool paren_r = !std::holds_alternative<Var>(ctx.get(node.right));

        if (paren_l) {
            std::print("(");
        }
        (*this)(node.left);
        if (paren_l) {
            std::print(")");
        }

        std::print(" ");

        if (paren_r) {
            std::print("(");
        }
        (*this)(node.right);
        if (paren_r) {
            std::print(")");
        }
    }

    auto operator()(const std::monostate &) const -> void
    {
        std::print("<\\0>");
    }
};

} // namespace lc::ast

auto print_ast(lc::ast::AstContext &ctx, lc::ast::NodeIdx ast)
{
    lc::ast::Printer{ctx}(ast);
    std::print("\n");
}

auto is_variable_free(lc::ast::AstContext &ctx, const std::string &name, lc::ast::NodeIdx tree_to_search) -> bool
{
    using namespace lc::ast;
    return std::visit(
        [&](const auto &search_space) -> bool {
            using T = std::decay_t<decltype(search_space)>;

            if constexpr (std::is_same_v<T, Var>) {
                return search_space.name == name;
            } else if constexpr (std::is_same_v<T, Abs>) {
                return search_space.param != name && is_variable_free(ctx, name, search_space.body);
            } else if constexpr (std::is_same_v<T, App>) {
                return is_variable_free(ctx, name, search_space.left) || is_variable_free(ctx, name, search_space.right);
            } else {
                return false;
            }
        },
        ctx.get(tree_to_search)); // Safe: is_variable_free doesn't allocate
}

struct Substitutor
{
    lc::ast::AstContext &ctx;
    std::string name;
    lc::ast::NodeIdx value;

    auto operator()(lc::ast::NodeIdx ast) const -> lc::ast::NodeIdx
    {
        auto node = ctx.get(ast);
        return std::visit([&](const auto &n) { return apply(ast, n); }, node);
    }

private:
    auto apply(lc::ast::NodeIdx self, const lc::ast::Var &v) const -> lc::ast::NodeIdx
    {
        return v.name == name ? value : self;
    }

    auto apply(lc::ast::NodeIdx self, const lc::ast::Abs &abs) const -> lc::ast::NodeIdx
    {
        if (abs.param == name) {
            return self;
        }

        if (!is_variable_free(ctx, abs.param, value)) {
            return ctx.make_abs(abs.param, (*this)(abs.body));
        }

        auto fresh = fresh_name(abs.param, abs.body);
        auto renamed_body = Substitutor{ctx, abs.param, ctx.make_var(fresh)}(abs.body);
        return ctx.make_abs(fresh, (*this)(renamed_body));
    }

    auto apply(lc::ast::NodeIdx, const lc::ast::App &app) const -> lc::ast::NodeIdx
    {
        return ctx.make_app((*this)(app.left), (*this)(app.right));
    }

    auto apply(lc::ast::NodeIdx self, const std::monostate &) const -> lc::ast::NodeIdx
    {
        return self;
    }

    auto fresh_name(const std::string &base, lc::ast::NodeIdx body) const -> std::string
    {
        std::string candidate = base;
        while (candidate == name || is_variable_free(ctx, candidate, value) || is_variable_free(ctx, candidate, body)) {
            candidate += '\'';
        }
        return candidate;
    }
};

struct Evaluator
{
    lc::ast::AstContext &ctx;

    auto operator()(lc::ast::NodeIdx ast) const -> lc::ast::NodeIdx
    {
        auto node = ctx.get(ast);
        return std::visit([&](const auto &n) { return eval(ast, n); }, node);
    }

private:
    auto eval(lc::ast::NodeIdx self, const lc::ast::Var &) const -> lc::ast::NodeIdx
    {
        return self;
    }

    auto eval(lc::ast::NodeIdx, const lc::ast::Abs &abs) const -> lc::ast::NodeIdx
    {
        return ctx.make_abs(abs.param, (*this)(abs.body));
    }

    auto eval(lc::ast::NodeIdx self, const std::monostate &) const -> lc::ast::NodeIdx
    {
        return self;
    }

    auto eval(lc::ast::NodeIdx, const lc::ast::App &app) const -> lc::ast::NodeIdx
    {
        auto fn = (*this)(app.left);

        auto fn_node = ctx.get(fn); // FIX: Copy evaluated function to local stack

        if (auto *abs = std::get_if<lc::ast::Abs>(&fn_node)) {
            return (*this)(Substitutor{ctx, abs->param, app.right}(abs->body));
        }

        if (auto *v = std::get_if<lc::ast::Var>(&fn_node); v && v->name == "print") {
            auto arg = (*this)(app.right);
            lc::ast::Printer{ctx}(arg);
            std::println();
            return arg;
        }

        return ctx.make_app(fn, (*this)(app.right));
    }
};

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
            Pos start{line, col};

            if (_index >= _code.size()) {
                out.push_back({Token_type::end, "", start});
                return out;
            }

            char c = _code[_index];

            // Parsing for '󰘧' (U+03BB := 0xCE 0xBB; 0b11001110 0b10111011).
            if (static_cast<unsigned char>(c) == 0xCE && _index + 1 < _code.size() && static_cast<unsigned char>(_code[_index + 1]) == 0xBB) {
                advance();
                advance();
                out.push_back({Token_type::backslash, "\\", start});
                continue;
            }

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

    auto advance() -> void
    {
        if (_code[_index] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        ++_index;
    }

    auto skip_whitespace() -> void
    {
        while (_index < _code.size() && std::isspace(static_cast<unsigned char>(_code[_index]))) {
            advance();
        }
    }

    auto identifier(Pos start) -> Token
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

class Parser
{
public:
    explicit Parser(std::span<const Token> t, ast::AstContext &ctx) : tokens(t), ctx(ctx)
    {}

    auto parse() -> ast::NodeIdx
    {
        auto result = term();
        expect(Token_type::end, "expected end of input");
        return result;
    }

private:
    std::span<const Token> tokens;
    std::size_t index{0};
    ast::AstContext &ctx;

    auto peek() const -> const Token &
    {
        return tokens[index];
    }

    auto advance() -> const Token &
    {
        const Token &t = tokens[index];
        if (t.type != Token_type::end) {
            ++index;
        }
        return t;
    }

    auto expect(Token_type t, std::string_view msg) -> const Token &
    {
        if (peek().type != t) {
            error(msg);
        }
        return advance();
    }

    [[noreturn]] auto error(std::string_view msg) const -> void
    {
        auto p = peek().pos;
        throw std::runtime_error(std::format("{}:{}: parse error: {}", p.l, p.c, msg));
    }

    auto starts_atom() const -> bool
    {
        auto k = peek().type;
        return k == Token_type::identifier || k == Token_type::paren_l || k == Token_type::backslash || k == Token_type::print;
    }

    auto term() -> ast::NodeIdx
    {
        auto left = atom();
        while (starts_atom()) {
            auto right = atom();
            left = ctx.make_app(left, right);
        }
        return left;
    }

    auto atom() -> ast::NodeIdx
    {
        switch (peek().type) {
        case Token_type::identifier: {
            return ctx.make_var(advance().text);
        } break;

        case Token_type::paren_l: {
            advance();
            auto inner = term();
            expect(Token_type::paren_r, "expected ')'");
            return inner;
        } break;

        case Token_type::backslash: {
            advance();
            std::string param = expect(Token_type::identifier, "expected parameter name").text;
            expect(Token_type::dot, "expected '.'");
            auto body = term();
            return ctx.make_abs(std::move(param), body);
        } break;

        case Token_type::print: {
            advance();
            auto operand = term();
            return ctx.make_app(ctx.make_var("print"), operand);
        } break;

        default:
            error("expected a term");
        }
    }
};
} // namespace lc::parse

auto print_usage(char *prog)
{
    std::println("Usage: ");
    std::println("  {} [file]", prog);
}

auto greet_message() -> std::string_view
{
    namespace stdch= std::chrono;

    std::time_t current_time = stdch::system_clock::to_time_t(stdch::system_clock::now());
    int hour = std::localtime(&current_time)->tm_hour;

    using namespace std::string_view_literals;
    if (hour >= 5 && hour < 18) {
        return "Have a nice day!"sv;
    } else {
        return "Good night!"sv;
    }
}

auto run_repl(lc::ast::AstContext &ctx) -> void
{
    std::println("Lambda Calculus REPL");
    std::println("Use '\\' or 'λ' for lambda, e.g. 'λx.x'");
    std::string line;

    while (true) {
        std::print("lc > ");

        if (!std::getline(std::cin, line)) {
            std::println("{}", greet_message());
            break;
        }

        if (line.empty()) {
            continue;
        }

        if (line == ":exit" || line == ":q") {
            std::println("{}", greet_message());
            break;
        }

        try {
            auto tokens = lc::parse::Lexer(line).tokenize();
            auto ast = lc::parse::Parser(tokens, ctx).parse();
            auto result = Evaluator{ctx}(ast);
            lc::ast::Printer{ctx}(result);
            std::println();
        } catch (const std::exception &e) {
            std::println("Error: {}", e.what());
        }
    }
}

auto main(int argc, char *argv[]) -> int
{
    std::array<std::byte, 256 * 1024> stack_buffer;
    std::pmr::monotonic_buffer_resource arena{stack_buffer.data(), stack_buffer.size(), std::pmr::new_delete_resource()};
    std::pmr::unsynchronized_pool_resource pool{&arena};

    lc::ast::AstContext ctx{&pool};

    if (argc < 2) {
        run_repl(ctx);
        return EXIT_SUCCESS;
    }

    try {
        if (!std::filesystem::exists(argv[1])) {
            throw std::invalid_argument("File doesn't exist");
        }

        std::fstream file(argv[1]);
        std::string code{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

        auto tokens = lc::parse::Lexer(code).tokenize();
        auto ast = lc::parse::Parser(tokens, ctx).parse();
        auto result = Evaluator{ctx}(ast);
        print_ast(ctx, result);

    } catch (const std::exception &e) {
        std::println(stderr, "Error: {}", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
