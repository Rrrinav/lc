// Command-line entry point for the lambda-calculus interpreter.
//
// This file contains three things:
//
//   1. The lexer and recursive-descent parser (`namespace lc::parse`).
//   2. Command-line option handling.
//   3. The REPL and file-running dispatch code.
//
// The AST, substitution, pretty-printer, and evaluator implementations live in
// separate headers under `src/`.
//
// Example program:
//
//     (\x.\y. x y) y
//
// The parser builds an application whose function is the lambda
// `\x.\y. x y` and whose argument is the outer `y`. Evaluation beta-reduces it
// to `\y'.y y'`, alpha-renaming the inner `y` so the outer `y` is not captured.

#include "ast.hpp"
#include "evaluator.hpp"
#include "explicit_stack_reducer.hpp"
#include "printer.hpp"
#include "recursive_tree_walker.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lc::parse {

// Token kinds produced by the lexer.
//
// For `(\x.\y. x y) y` the token stream is roughly:
//
//     paren_l, backslash, identifier("x"), dot,
//     backslash, identifier("y"), dot,
//     identifier("x"), identifier("y"),
//     paren_r, identifier("y"), end
//
// `print` is a keyword token because it is a built-in, not an ordinary variable.
enum Token_type { backslash, identifier, dot, paren_l, paren_r, end, print };

// 1-based source position, used in lexer and parser error messages.
struct Pos
{
    std::uint16_t l{}, c{};
};

// One lexed token: its kind, original text, and position.
struct Token
{
    Token_type type;
    std::string text;
    Pos pos;
};

// Turns source text into tokens.
//
// Supported syntax:
//   - `\` or `λ` starts a lambda
//   - identifiers may contain letters, digits, `_`, and `'`
//   - `.`, `(`, `)`, and whitespace
//
// Identifiers ending in apostrophes are ordinary names, which is why alpha
// renaming can produce names such as `y'` and `y''`.
class Lexer
{
public:
    explicit Lexer(std::string_view code) : _code(code)
    {}

    // Tokenize the whole input and append a final `end` token.
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

    // Move one byte forward, updating the line and column counters.
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

    // Skip spaces, tabs, and newlines between tokens.
    auto skip_whitespace() -> void
    {
        while (_index < _code.size() && std::isspace(static_cast<unsigned char>(_code[_index]))) {
            advance();
        }
    }

    // Read a complete identifier. `print` becomes the built-in keyword token;
    // everything else becomes an ordinary identifier.
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

    // Identifiers must start with a letter or underscore.
    static auto is_ident_start(char c) -> bool
    {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }

    // After the first character, digits and apostrophes are allowed too.
    // Apostrophes let alpha-renamed names such as `y'` be parsed normally.
    static auto is_ident_char(char c) -> bool
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '\'';
    }
};

// Recursive-descent parser for lambda terms.
//
// Grammar (informally):
//
//     term  := atom atom*          // application is left-associative
//     atom  := identifier
//            | '(' term ')'
//            | '\' identifier '.' term
//            | 'print' term
//
// Examples:
//
//     x y z        -> App(App(x, y), z)
//     \x.x y       -> Abs("x", App(x, y))
//     (\x.x) y     -> App(Abs("x", x), y)
//     print x      -> App(Var("print"), x)
//
// Because application is parsed left-associatively, `(\x.\y. x y) y` becomes an
// application of the whole lambda to the final `y`, not an application inside
// the lambda body.
class Parser
{
public:
    explicit Parser(std::span<const Token> t, ast::Ast_context &ctx) : tokens(t), ctx(ctx)
    {}

    // Parse one complete term and require the token stream to end afterwards.
    auto parse() -> ast::Node_id
    {
        auto result = term();
        expect(Token_type::end, "expected end of input");
        return result;
    }

private:
    std::span<const Token> tokens;
    std::size_t index{0};
    ast::Ast_context &ctx;

    // Look at the next token without consuming it.
    auto peek() const -> const Token &
    {
        return tokens[index];
    }

    // Consume and return the next token. The final `end` token is not consumed.
    auto advance() -> const Token &
    {
        const Token &t = tokens[index];
        if (t.type != Token_type::end) {
            ++index;
        }
        return t;
    }

    // Consume a token of the expected kind, or report a parse error.
    auto expect(Token_type t, std::string_view msg) -> const Token &
    {
        if (peek().type != t) {
            error(msg);
        }
        return advance();
    }

    // Throw a parse error at the current token's position.
    [[noreturn]] auto error(std::string_view msg) const -> void
    {
        auto p = peek().pos;
        throw std::runtime_error(std::format("{}:{}: parse error: {}", p.l, p.c, msg));
    }

    // True when the next token can begin an atom. Used to keep consuming
    // arguments in an application.
    auto starts_atom() const -> bool
    {
        auto k = peek().type;
        return k == Token_type::identifier || k == Token_type::paren_l || k == Token_type::backslash || k == Token_type::print;
    }

    // Parse a sequence of atoms as a left-associative application:
    //
    //     f x y   ->  App(App(f, x), y)
    auto term() -> ast::Node_id
    {
        auto left = atom();
        while (starts_atom()) {
            auto right = atom();
            left = ctx.make_app(left, right);
        }
        return left;
    }

    // Parse one atom: a variable, a parenthesized term, a lambda, or `print`.
    auto atom() -> ast::Node_id
    {
        switch (peek().type) {
        case Token_type::identifier: {
            // A bare name becomes a variable node.
            return ctx.make_var(advance().text);
        } break;

        case Token_type::paren_l: {
            // Parentheses only group; they do not create AST nodes.
            advance();
            auto inner = term();
            expect(Token_type::paren_r, "expected ')'");
            return inner;
        } break;

        case Token_type::backslash: {
            // `\x.body` becomes Abs("x", body).
            advance();
            std::string param = expect(Token_type::identifier, "expected parameter name").text;
            expect(Token_type::dot, "expected '.'");
            auto body = term();
            return ctx.make_abs(std::move(param), body);
        } break;

        case Token_type::print: {
            // The built-in is represented as an application of a special
            // variable named "print": `print x` becomes App(Var("print"), x).
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

// Parsed command-line options.
//
// Examples:
//
//     lc file.lc
//         engine = recursive, trace = false
//
//     lc file.lc --evaluator stack
//         engine = stack, trace = false
//
//     lc file.lc --trace
//         engine = stack, trace = true
//
//     lc file.lc --evaluator recursive --trace
//         error: tracing is only supported by the stack evaluator
struct Options
{
    enum class Engine { recursive, stack };

    bool trace{};
    bool help{};
    bool engine_explicit{};
    Engine engine{Engine::recursive};
    std::optional<std::string> file;
};

// Print the command-line help text.
auto print_usage(char *prog) -> void
{
    std::println("Usage: {} [options] [file]", prog);
    std::println();
    std::println("Options:");
    std::println("  -e, --evaluator <name>  recursive (default) or stack");
    std::println("  -t, --trace             show whole-term reduction steps (stack evaluator)");
    std::println("  -h, --help              show this help");
}

// Parse arguments. The input file may appear before or after options, so both
// `lc file.lc --trace` and `lc --trace file.lc` work.
auto parse_options(int argc, char *argv[]) -> Options
{
    Options options;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];

        if (arg == "--trace" || arg == "-t") {
            options.trace = true;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--evaluator" || arg == "-e") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for --evaluator");
            }

            std::string_view value = argv[++i];
            options.engine_explicit = true;

            if (value == "recursive") {
                options.engine = Options::Engine::recursive;
            } else if (value == "stack") {
                options.engine = Options::Engine::stack;
            } else {
                throw std::invalid_argument(std::format("unknown evaluator: {}", value));
            }
        } else if (arg.starts_with('-')) {
            throw std::invalid_argument(std::format("unknown option: {}", arg));
        } else if (options.file.has_value()) {
            throw std::invalid_argument("only one input file may be given");
        } else {
            options.file = std::string(arg);
        }
    }

    // `--trace` needs whole-term steps, which only the stack evaluator provides.
    // If the user did not explicitly choose an engine, tracing selects the
    // stack engine automatically.
    if (options.trace) {
        if (options.engine_explicit && options.engine == Options::Engine::recursive) {
            throw std::invalid_argument("--trace is only supported by the stack evaluator");
        }
        options.engine = Options::Engine::stack;
    }

    return options;
}

// A small time-of-day greeting used when leaving the REPL.
auto greet_message() -> std::string_view
{
    std::time_t current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    int hour = std::localtime(&current_time)->tm_hour;

    using namespace std::string_view_literals;
    if (hour >= 5 && hour < 18) {
        return "Have a nice day!"sv;
    } else {
        return "Good night!"sv;
    }
}

// Evaluate one parsed term and print its result.
//
// The selected strategy is a compile-time choice:
//
//   - trace mode always uses `explicit_stack_reducer`;
//   - otherwise `--evaluator stack` selects the stack machine;
//   - the default is the original `recursive_tree_walker`.
//
// `trace_execution` prints the result as its last trace line, so we do not print
// the result a second time in trace mode.
auto evaluate_and_print(lc::ast::Ast_context &ctx, lc::ast::Node_id ast, const Options &options) -> void
{
    if (options.trace) {
        lc::Evaluator<lc::explicit_stack_reducer>{ctx}.trace_execution(ast);
        return;
    }

    if (options.engine == Options::Engine::stack) {
        auto result = lc::Evaluator<lc::explicit_stack_reducer>{ctx}(ast);
        print_ast(ctx, result);
    } else {
        auto result = lc::Evaluator<lc::recursive_tree_walker>{ctx}(ast);
        print_ast(ctx, result);
    }
}

// Interactive read-eval-print loop.
//
// Example session:
//
//     lc > (\x.\y. x y) y
//     \y'.y y'
//     lc > :q
//     Have a nice day!
//
// All input lines share the same `Ast_context`, so the arena keeps growing as
// more terms are parsed and evaluated.
auto run_repl(lc::ast::Ast_context &ctx, const Options &options) -> void
{
    std::println("-- Lambda Calculus REPL <github.com/rrrinav/lc>");
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
            evaluate_and_print(ctx, ast, options);
        } catch (const std::exception &e) {
            std::println("Error: {}", e.what());
        }
    }
}

// Program entry point.
//
// Execution flow:
//   1. Create a memory arena and AST context.
//   2. Parse command-line options.
//   3. If no file was given, run the REPL.
//   4. Otherwise read the file, lex, parse, evaluate, and print.
auto main(int argc, char *argv[]) -> int
{
    // A small fixed buffer backs the monotonic arena. If it runs out, the arena
    // falls back to `new_delete_resource`.
    std::array<std::byte, 256 * 1024> stack_buffer;
    std::pmr::monotonic_buffer_resource arena{stack_buffer.data(), stack_buffer.size(), std::pmr::new_delete_resource()};
    std::pmr::unsynchronized_pool_resource pool{&arena};

    // Owns every AST node for the lifetime of the program.
    lc::ast::Ast_context ctx{&pool};

    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception &e) {
        std::println(stderr, "Error: {}", e.what());
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (options.help) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    // No input file: interactive mode.
    if (!options.file.has_value()) {
        run_repl(ctx, options);
        return EXIT_SUCCESS;
    }

    try {
        if (!std::filesystem::exists(*options.file)) {
            throw std::invalid_argument("File doesn't exist");
        }

        // Read the whole file into memory. The lexer takes a string_view, so the
        // string must stay alive until parsing is finished.
        std::fstream file(*options.file);
        std::string code{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

        auto tokens = lc::parse::Lexer(code).tokenize();
        auto ast = lc::parse::Parser(tokens, ctx).parse();
        evaluate_and_print(ctx, ast, options);

    } catch (const std::exception &e) {
        std::println(stderr, "Error: {}", e.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
