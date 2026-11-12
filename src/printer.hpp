#pragma once

// Pretty-printer for AST terms.
//
// The printer always writes to an `std::ostream`, so the same code is used for
// normal program output (`std::cout`) and for traces (`out` can be any stream).
//
// Printing rules:
//   - Application is left-associative: `f x y` means `(f x) y`.
//   - The function side of an application is parenthesized when it is a lambda.
//   - The argument side is parenthesized when it is not a plain variable.
//
// Examples:
//
//     App(Abs("x", Var("x")), Var("y"))      -> "(\x.x) y"
//     App(Var("f"), Abs("x", Var("x")))      -> "f (\x.x)"
//     App(App(Var("f"), Var("x")), Var("y")) -> "f x y"
//     App(Var("f"), App(Var("x"), Var("y"))) -> "f (x y)"

#include "ast.hpp"

#include <iostream>
#include <ostream>
#include <variant>

namespace lc::ast {

struct Printer
{
    Ast_context &ctx;
    std::ostream &out;

    // Dispatch on the kind of node stored at `ast`.
    auto operator()(Node_id ast) const -> void
    {
        std::visit(*this, ctx.get(ast));
    }

    // A variable prints as its name, for example `x`.
    auto operator()(const Var &node) const -> void
    {
        out << node.name;
    }

    // An abstraction prints as `\param.body`, for example `\x.x y`.
    auto operator()(const Abs &node) const -> void
    {
        out << '\\' << node.param << '.';
        (*this)(node.body);
    }

    // Application printing. The parentheses below preserve the grouping that
    // the AST already encodes:
    //
    //   `(\x.\y. x y) y` needs parentheses around the lambda because it is the
    //   function being applied. Without them, `\x.\y. x y y` would parse as a
    //   different term.
    auto operator()(const App &node) const -> void
    {
        bool paren_l = std::holds_alternative<Abs>(ctx.get(node.left));
        bool paren_r = !std::holds_alternative<Var>(ctx.get(node.right));

        if (paren_l) {
            out << '(';
        }
        (*this)(node.left);
        if (paren_l) {
            out << ')';
        }

        out << ' ';

        if (paren_r) {
            out << '(';
        }
        (*this)(node.right);
        if (paren_r) {
            out << ')';
        }
    }

    // Placeholder node used when no real term is available.
    auto operator()(const std::monostate &) const -> void
    {
        out << "<\\0>";
    }
};

} // namespace lc::ast

// Convenience wrapper for the common case: print to stdout and finish the line.
inline auto print_ast(lc::ast::Ast_context &ctx, lc::ast::Node_id ast) -> void
{
    lc::ast::Printer{ctx, std::cout}(ast);
    std::cout << '\n';
}
