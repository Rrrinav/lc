#pragma once

// The classic recursive tree-walking evaluator.
//
// This is the reference implementation. It mirrors the way beta reduction is
// usually described on paper:
//
//   - A variable is already a value.
//   - An abstraction evaluates its body under the binder, so the result is a
//     full normal form, not just a weak-head normal form.
//   - An application evaluates the function side first. If that side is a
//     lambda, the evaluator substitutes the (unevaluated) argument into the body
//     and evaluates the result. This is call-by-name: the argument is only
//     evaluated if it is actually used.
//
// Example:
//
//     (\x.\y. x y) y
//
//   1. The function side `\x.\y. x y` is already an abstraction.
//   2. Substitute `x := y` into the body `\y. x y`.
//   3. The inner `y` would capture the outer `y`, so alpha-rename it:
//
//          \y. x y  ->  \y'. x y'  ->  \y'. y y'
//
//   4. Evaluate the resulting body under the binder. It is already normal.
//
//   Final result: `\y'. y y'`.
//
// Call-by-name example:
//
//     (\x.y) (print z)
//
// `x` does not occur in the body, so the argument `(print z)` is discarded and
// `z` is never printed.
//
// The recursion here uses the C++ call stack. `explicit_stack_reducer` performs
// the same evaluation with an explicit stack instead.

#include "ast.hpp"
#include "printer.hpp"
#include "substitution.hpp"

#include <string>
#include <variant>

namespace lc {

struct recursive_tree_walker
{
    // Public entry point used by `Evaluator<recursive_tree_walker>`.
    auto evaluate(ast::Ast_context &ctx, ast::Node_id ast) const -> ast::Node_id
    {
        return eval(ctx, ast);
    }

private:
    // Dispatch on the kind of node and call the matching `eval` overload.
    auto eval(ast::Ast_context &ctx, ast::Node_id ast) const -> ast::Node_id
    {
        // Copy the node before recursing. Evaluating may append nodes to the
        // arena, which can invalidate references returned by `ctx.get`.
        auto node = ctx.get(ast);
        return std::visit([&](const auto &n) { return eval(ctx, ast, n); }, node);
    }

    // A variable is already in normal form.
    auto eval(ast::Ast_context &, ast::Node_id self, const ast::Var &) const -> ast::Node_id
    {
        return self;
    }

    // Evaluate under the binder. For example, the body of `\x.(\y.y) z` is
    // reduced to `\x.z`, so the final result is a normal form inside the lambda
    // as well.
    auto eval(ast::Ast_context &ctx, ast::Node_id, const ast::Abs &abs) const -> ast::Node_id
    {
        return ctx.make_abs(abs.param, eval(ctx, abs.body));
    }

    // Placeholder nodes are already in normal form.
    auto eval(ast::Ast_context &, ast::Node_id self, const std::monostate &) const -> ast::Node_id
    {
        return self;
    }

    // Application: evaluate the function side first, then decide what to do.
    auto eval(ast::Ast_context &ctx, ast::Node_id, const ast::App &app) const -> ast::Node_id
    {
        auto fn = eval(ctx, app.left);

        // Copy the function node: the substitution below can append to the
        // arena and reallocate its storage, which would dangle a reference.
        auto fn_node = ctx.get(fn);

        // Beta reduction:
        //
        //     (\x. body) argument  ->  body[x := argument]
        //
        // The argument is not evaluated first (call-by-name). Substitution
        // performs alpha-renaming when needed, so `(\x.\y. x y) y` becomes
        // `\y'. y y'` rather than the incorrect `\y. y y`.
        if (auto *abs = std::get_if<ast::Abs>(&fn_node)) {
            return eval(ctx, Substitutor{ctx, abs->param, app.right}(abs->body));
        }

        // Built-in `print`:
        //
        //     print value  ->  value
        //
        // Unlike a normal lambda application, the argument is evaluated to a
        // normal form first, then printed as a side effect.
        if (auto *v = std::get_if<ast::Var>(&fn_node); v && v->name == "print") {
            auto arg = eval(ctx, app.right);
            print_ast(ctx, arg);
            return arg;
        }

        // Stuck application: the function side is not a lambda and not `print`.
        // Evaluate the argument and rebuild the application, for example
        // `x ((\y.y) z)` becomes `x z`.
        return ctx.make_app(fn, eval(ctx, app.right));
    }
};

} // namespace lc
