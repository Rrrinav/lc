#pragma once

// Strategy-based evaluator wrapper.
//
// The evaluation algorithm is selected as a template parameter, so the rest of
// the program can switch between implementations without changing its code:
//
//     Evaluator<recursive_tree_walker>       // original recursive evaluator
//     Evaluator<explicit_stack_reducer>      // explicit-stack machine
//
// Both strategies receive the same `Ast_context` and `Node_id`, so they produce
// the same normal form:
//
//     (\x.\y. x y) y   ->   \y'.y y'
//
// The difference is how they get there:
//
//   - `recursive_tree_walker` uses the C++ call stack and does not expose
//     intermediate terms.
//   - `explicit_stack_reducer` uses a frame stack and can print every complete
//     term after each reduction through `trace_execution`.

#include "ast.hpp"

#include <concepts>
#include <iostream>
#include <ostream>

namespace lc {

// Every evaluator strategy must provide:
//
//     auto evaluate(ast::Ast_context &ctx, ast::Node_id ast) const
//         -> ast::Node_id;
//
// `ctx` owns the AST arena, `ast` is the root term, and the return value is the
// normal form. The strategy is stateless, so it can be default-constructed.
template <typename Strategy>
concept Evaluator_strategy = requires(
    const Strategy &strategy,
    ast::Ast_context &ctx,
    ast::Node_id ast)
{
    { strategy.evaluate(ctx, ast) } -> std::same_as<ast::Node_id>;
};

// A traceable strategy additionally provides:
//
//     auto trace_execution(ast::Ast_context &ctx, ast::Node_id ast,
//                          std::ostream &out) const -> ast::Node_id;
//
// This is what makes whole-term tracing possible. The recursive walker does not
// satisfy this concept because it does not produce complete intermediate terms.
template <typename Strategy>
concept Traceable_evaluator_strategy =
    Evaluator_strategy<Strategy> && requires(
        const Strategy &strategy,
        ast::Ast_context &ctx,
        ast::Node_id ast,
        std::ostream &out)
{
    { strategy.trace_execution(ctx, ast, out) } -> std::same_as<ast::Node_id>;
};

// Thin wrapper around a strategy. It keeps the AST context and forwards calls.
//
// Example:
//
//     lc::ast::Ast_context ctx{&pool};
//     auto ast = ...;  // parsed term
//
//     auto result =
//         lc::Evaluator<lc::recursive_tree_walker>{ctx}(ast);
//
//     lc::Evaluator<lc::explicit_stack_reducer>{ctx}.trace_execution(ast);
template <Evaluator_strategy Strategy>
class Evaluator
{
public:
    explicit Evaluator(ast::Ast_context &ctx) : _ctx(ctx) {}

    // Normal evaluation. This is available for every strategy.
    auto operator()(ast::Node_id ast) const -> ast::Node_id
    {
        return _strategy.evaluate(_ctx, ast);
    }

    // Whole-term tracing. Only available when the strategy supports it, so
    // `Evaluator<recursive_tree_walker>{ctx}.trace_execution(ast)` fails at
    // compile time with a clear constraint error.
    //
    // `out` defaults to stdout:
    //
    //     Evaluator<explicit_stack_reducer>{ctx}.trace_execution(ast);
    auto trace_execution(ast::Node_id ast, std::ostream &out = std::cout) const -> ast::Node_id
        requires Traceable_evaluator_strategy<Strategy>
    {
        return _strategy.trace_execution(_ctx, ast, out);
    }

private:
    ast::Ast_context &_ctx;
    [[no_unique_address]] Strategy _strategy{};
};

} // namespace lc
