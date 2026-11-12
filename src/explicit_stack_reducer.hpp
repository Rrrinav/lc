#pragma once

// An explicit-stack, one-step reducer with whole-term tracing.
//
// This evaluator has the same semantics as `recursive_tree_walker`, but it uses
// an explicit `std::vector<Frame>` instead of the C++ call stack. That makes the
// control flow visible and lets us return a complete term after every reduction.
//
// The core is `reduce_one`:
//
//   - Walk down the leftmost function positions, pushing frames that remember
//     what still has to be rebuilt.
//   - When a redex is found, perform exactly one beta reduction (or one `print`
//     step) and rebuild the whole term from the saved frames.
//   - Return that whole term. `run` repeats until `reduce_one` finds nothing.
//
// Because each step returns the full root term, the pen-and-paper trace is just
// a matter of printing it:
//
//     (\x.\y. x y) y
//     - \y'.y y'   (α: y → y')
//
// Worked example for `(\x.\y. x y) y`:
//
//   1. Descend through the outer App: push `App_left{y}`, focus `\x.\y. x y`.
//   2. Descend through the outer Abs: push `Abs_body{x}`, focus `\y. x y`.
//   3. Descend through the inner Abs: push `Abs_body{y}`, focus `x y`.
//   4. Descend through the inner App: push `App_left{y}`, focus `x`.
//      `x` is normal. It is not a lambda or `print`, so replace the frame with
//      `App_right{x}` and descend into `y`. `y` is normal, so rebuild `x y`,
//      then `\y. x y`, then `\x.\y. x y`.
//   5. Back at the outer `App_left{y}`: the function side is a lambda, so beta
//      reduce. Substituting `x := y` into `\y. x y` would capture the outer `y`,
//      so alpha-rename it first:
//
//          \y. x y  ->  \y'. x y'  ->  \y'. y y'
//
//   6. Rebuild the root and return the new term. The next `reduce_one` call
//      finds no further redex, so `\y'. y y'` is the normal form.

#include "ast.hpp"
#include "printer.hpp"
#include "substitution.hpp"

#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lc::detail {
// We are descending into the function side of `App(left, right)`.
// `right` is the argument we will need after the function side is normal.
struct App_left
{
    ast::Node_id right;
};

// The function side is normal and is not a lambda or `print`.
// We are now descending into the argument side of `App(left, right)`.
struct App_right
{
    ast::Node_id left;
};

// We are descending into the body of `\param. body`.
struct Abs_body
{
    std::string param;
};

// We are evaluating the argument of the built-in `print`.
// The argument replaces the whole `print argument` application when it is done.
struct Print_arg
{};

// A frame remembers one step on the path from the root to the current node.
// Rebuilding walks this path backwards.
using Frame = std::variant<App_left, App_right, Abs_body, Print_arg>;

// The result of one reduction step.
struct Reduction
{
    // The complete root term after the step, ready to be printed.
    ast::Node_id term;

    // True when this step was the `print` builtin rather than a beta reduction.
    bool printed{};

    // Alpha-renames performed while substituting, for trace annotations such as
    // `(α: y → y')`.
    std::vector<std::pair<std::string, std::string>> renames;
};

// Rebuild the root from a stack of frames.
//
// `current` is the node that has just been completed. Each frame describes the
// parent node that contains it:
//
//   App_left{right}  ->  current is the left side:   App(current, right)
//   App_right{left}  ->  current is the right side:  App(left, current)
//   Abs_body{param}  ->  current is the body:        \param.current
//   Print_arg        ->  current is the printed arg: current (the argument is
//                        the result of `print argument`)
inline auto rebuild(ast::Ast_context &ctx, std::vector<Frame> &stack, ast::Node_id current) -> ast::Node_id
{
    while (!stack.empty()) {
        auto frame = std::move(stack.back());
        stack.pop_back();

        current = std::visit(
            [&](const auto &f) -> ast::Node_id {
                using T = std::decay_t<decltype(f)>;

                if constexpr (std::is_same_v<T, App_right>) {
                    return ctx.make_app(f.left, current);
                } else if constexpr (std::is_same_v<T, App_left>) {
                    return ctx.make_app(current, f.right);
                } else if constexpr (std::is_same_v<T, Abs_body>) {
                    return ctx.make_abs(f.param, current);
                } else if constexpr (std::is_same_v<T, Print_arg>) {
                    return current;
                } else {
                    throw std::logic_error("invalid frame while rebuilding reduction");
                }
            },
            frame);
    }

    return current;
}

// Find and perform exactly one reduction, then return the rebuilt root term.
//
// Returns `std::nullopt` when the term is already in normal form.
//
// The walk order is the same as `recursive_tree_walker`: normalise the function
// side first, then beta-reduce, otherwise normalise the argument. Binders are
// also normalised, so the result is a full normal form.
inline auto reduce_one(ast::Ast_context &ctx, ast::Node_id root) -> std::optional<Reduction>
{
    std::vector<Frame> stack;
    ast::Node_id current = root;

    while (true) {
        // Copy the node before descending or allocating: the arena vector can
        // reallocate and invalidate references.
        auto node = ctx.get(current);

        if (auto *app = std::get_if<ast::App>(&node)) {
            // Descend into the function side first.
            stack.push_back(App_left{app->right});
            current = app->left;
            continue;
        }

        if (auto *abs = std::get_if<ast::Abs>(&node)) {
            // Normalise under the binder, just like the recursive evaluator.
            stack.push_back(Abs_body{abs->param});
            current = abs->body;
            continue;
        }

        // `current` is a Var or monostate, so this subterm is normal. Unwind
        // through the stack until we can either rebuild the parent or descend
        // into a sibling that still needs work.
        bool descend = false;
        while (!descend) {
            if (stack.empty()) {
                // The whole term is normal.
                return std::nullopt;
            }

            auto frame = std::move(stack.back());
            stack.pop_back();

            if (auto *app_left = std::get_if<App_left>(&frame)) {
                // The function side is normal. Decide what to do with it.
                auto left_node = ctx.get(current);

                if (auto *abs = std::get_if<ast::Abs>(&left_node)) {
                    // Beta reduction: (\param. body) argument.
                    // Substitution can rename binders, so collect the renames
                    // for the trace annotation.
                    std::vector<std::pair<std::string, std::string>> renames;
                    auto substituted = Substitutor{ctx, abs->param, app_left->right, &renames}(abs->body);
                    return Reduction{rebuild(ctx, stack, substituted), false, std::move(renames)};
                }

                if (auto *var = std::get_if<ast::Var>(&left_node); var && var->name == "print") {
                    // Built-in print: evaluate the argument first.
                    stack.push_back(Print_arg{});
                    current = app_left->right;
                    descend = true;
                } else {
                    // Stuck function: evaluate the argument and rebuild the
                    // application, for example `x ((\y.y) z)` -> `x z`.
                    stack.push_back(App_right{current});
                    current = app_left->right;
                    descend = true;
                }
            } else if (auto *app_right = std::get_if<App_right>(&frame)) {
                // The argument is normal: rebuild the application and continue
                // unwinding into its parent.
                current = ctx.make_app(app_right->left, current);
            } else if (auto *abs_body = std::get_if<Abs_body>(&frame)) {
                // The body is normal: rebuild the abstraction and continue
                // unwinding into its parent.
                current = ctx.make_abs(abs_body->param, current);
            } else {
                // Print_arg: the argument is normal. `print argument` reduces
                // to `argument`, so the current node is the result.
                return Reduction{rebuild(ctx, stack, current), true, {}};
            }
        }
    }
}

} // namespace lc::detail

namespace lc {

// Strategy used by `Evaluator<explicit_stack_reducer>`.
//
// `evaluate` runs without tracing. `trace_execution` runs the same machine and
// prints every complete term after each step.
struct explicit_stack_reducer
{
    auto evaluate(ast::Ast_context &ctx, ast::Node_id ast) const -> ast::Node_id
    {
        return run(ctx, ast, nullptr);
    }

    // `out` defaults to stdout, so the common case is simply:
    //
    //     Evaluator<explicit_stack_reducer>{ctx}.trace_execution(ast);
    auto trace_execution(ast::Ast_context &ctx, ast::Node_id ast, std::ostream &out = std::cout) const -> ast::Node_id
    {
        return run(ctx, ast, &out);
    }

private:
    // Shared driver for both normal evaluation and tracing. `trace == nullptr`
    // means "do not print steps"; otherwise every step is written to `*trace`.
    static auto run(ast::Ast_context &ctx, ast::Node_id ast, std::ostream *trace) -> ast::Node_id
    {
        auto current = ast;

        // Pen-and-paper style: start with the original term.
        if (trace != nullptr) {
            ast::Printer{ctx, *trace}(current);
            *trace << '\n';
        }

        while (auto reduction = detail::reduce_one(ctx, current)) {
            current = reduction->term;

            if (trace != nullptr) {
                // Each reduction is printed as `-> <complete term>`.
                *trace << "=> ";
                ast::Printer{ctx, *trace}(current);

                if (reduction->printed) {
                    // In trace mode the printed value is already visible in the
                    // term, so annotate instead of printing it a second time.
                    *trace << "   (print)";
                } else if (!reduction->renames.empty()) {
                    // Example: `(α: y -> y')` after capture avoidance.
                    *trace << "   (";
                    bool first = true;
                    for (const auto &[old_name, fresh] : reduction->renames) {
                        if (!first) {
                            *trace << ", ";
                        }
                        *trace << "α: " << old_name << " → " << fresh;
                        first = false;
                    }
                    *trace << ")";
                }

                *trace << '\n';
            } else if (reduction->printed) {
                // Normal mode preserves the usual `print` side effect.
                // For example, `print (\x.x)` prints `\x.x` and then returns it,
                // so the caller may print the result again.
                ast::Printer{ctx, std::cout}(current);
                std::cout << '\n';
            }
        }

        return current;
    }
};

} // namespace lc
