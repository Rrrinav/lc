#pragma once

// Free-variable checking and capture-avoiding substitution.
//
// These are the two operations the evaluators use to perform beta reduction:
//
//     (\x. body) arg   ->   body[x := arg]
//
// The evaluator calls:
//
//     Substitutor{ctx, "x", arg}(body)
//
// with:
//     name        = "x"    the variable being replaced
//     replacement = arg    the term inserted in its place
//
// `replacement` is the *argument*, not the body. Every capture check below asks
// about the argument and never about the body.
//
// In one line, Substitutor{ctx, name, replacement}(tree):
//   - replaces every free occurrence of `name` in `tree` with `replacement`
//   - stops at a binder that shadows `name`
//   - alpha-renames a binder whose name occurs free in `replacement`
//   - does not rename because of names that are bound inside `tree`
//
// --------------------------------------------------------------------------
// Demo run 1: capture is possible, so the binder is renamed
// --------------------------------------------------------------------------
//
// Evaluate:
//
//     (\x.\y. x y) y
//
// The evaluator sees:
//
//     App(Abs("x", Abs("y", App(Var("x"), Var("y")))), Var("y"))
//
// and calls:
//
//     Substitutor{ctx, "x", Var("y")}   on   Abs("y", App(Var("x"), Var("y")))
//
//     name        = "x"
//     replacement = Var("y")              <- the bare free argument
//
// Walking the body `\y. x y`:
//
//     apply(Abs("y", ...))
//       abs.param "y" != name "x"          -> the binder does not shadow x
//       variable_occurs_free_in(ctx, "y", replacement) == true
//       because `replacement` is the bare y, so inserting it under \y would
//       capture it. Rename the binder first:
//
//     \y. x y  --(rename y -> y')-->  \y'. x y'  --(x := y)-->  \y'. y y'
//
// It then makes two substitution passes:
//
//     Substitutor{ctx, "y", Var("y'")}   renames bound y in the body
//     Substitutor{ctx, "x", Var("y")}    inserts the argument
//
// Result: `\y'. y y'`. Without the rename we would get `\y. y y`, where the
// inserted `y` is wrongly bound by the inner lambda.
//
// --------------------------------------------------------------------------
// Demo run 2: no free name in the replacement, so no rename
// --------------------------------------------------------------------------
//
// Evaluate:
//
//     (\x.\y. x y) (\y.y)
//
// Now the call is:
//
//     Substitutor{ctx, "x", Abs("y", Var("y"))}
//
//     name        = "x"
//     replacement = Abs("y", Var("y"))    <- y is bound inside the argument
//
// Walking the body `\y. x y`:
//
//     abs.param "y" != name "x"          -> the binder does not shadow x
//     variable_occurs_free_in(ctx, "y", replacement) == false
//     because the y in `\y.y` is bound by the argument's own lambda. It cannot
//     be captured, so the binder stays:
//
//     substitution:  \y. x y  --(x := \y.y)-->  \y. (\y.y) y
//     later beta:    \y. (\y.y) y  -->  \y.y
//
// The walk does visit the body `\y. x y`, but the question "is y free?" is
// asked about `replacement`. A y bound in the body says nothing about capture.

#include "ast.hpp"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Returns true when `name` occurs free in `tree_to_search`.
//
// The check is always about one concrete tree. It does not look at any larger
// term the tree might later be inserted into. Examples:
//
//     variable_occurs_free_in(ctx, "y", \x.\y. x y)  -> false  (bound by inner \y)
//     variable_occurs_free_in(ctx, "x", \x.\y. x y)  -> false  (bound by outer \x)
//     variable_occurs_free_in(ctx, "z", \x.\y. x y)  -> true   (z is not present)
//
// The App case checks both sides; the Abs case stops at the binder that matches
// the name, because occurrences under that binder refer to the bound variable.
inline auto variable_occurs_free_in(lc::ast::Ast_context &ctx, const std::string &name, lc::ast::Node_id tree_to_search) -> bool
{
    using namespace lc::ast;
    return std::visit(
        [&](const auto &search_space) -> bool {
            using T = std::decay_t<decltype(search_space)>;

            if constexpr (std::is_same_v<T, Var>) {
                // If the name inside the tree and name match, free variable exists
                return search_space.name == name;
            } else if constexpr (std::is_same_v<T, Abs>) {
                // If the param name is not same, then it is free
                return search_space.param != name && variable_occurs_free_in(ctx, name, search_space.body);
            } else if constexpr (std::is_same_v<T, App>) {
                return variable_occurs_free_in(ctx, name, search_space.left) || variable_occurs_free_in(ctx, name, search_space.right);
            } else {
                return false;
            }
        },
        ctx.get(tree_to_search)); // Safe: variable_occurs_free_in doesn't allocate
}

// Capture-avoiding substitution: replace free occurrences of `name` in an AST
// with a replacement term.
//
// Example:
//
//     Substitutor{ctx, "x", ctx.make_var("y")}(body_of(\x.\y. x y))
//
// produces the term `\y'. y y'`: the inner binder `y` is renamed because the
// replacement term `y` would otherwise be captured.
//
// Shadowing example:
//
//     Substitutor{ctx, "x", replacement}(body_of(\x. x))  ->  \x. x
//
// The inner `\x` shadows the outer `x`, so the bound occurrence is left alone.
struct Substitutor
{
    lc::ast::Ast_context &ctx;
    std::string name;
    lc::ast::Node_id replacement;

    // Optional output parameter used by the trace renderer. Whenever an
    // alpha-rename happens, `(old_name, fresh_name)` is appended so the trace
    // can show annotations such as `(α: y -> y')`.
    //
    // Normal evaluation leaves this as nullptr and pays no extra cost.
    std::vector<std::pair<std::string, std::string>> *renames{};

    auto operator()(lc::ast::Node_id ast) const -> lc::ast::Node_id
    {
        // Copy the node before recursing. Substitution appends new nodes to the
        // arena, which may reallocate its vector and invalidate references.
        auto node = ctx.get(ast);
        return std::visit([&](const auto &n) { return apply(ast, n); }, node);
    }

private:
    // Variable case: replace it only when the names match.
    auto apply(lc::ast::Node_id self, const lc::ast::Var &v) const -> lc::ast::Node_id
    {
        return v.name == name ? replacement : self;
    }

    // Abstraction case: substitute under the binder unless the binder shadows
    // `name`. If the replacement contains the binder's name free, rename the
    // binder first so the replacement cannot be captured.
    auto apply(lc::ast::Node_id self, const lc::ast::Abs &abs) const -> lc::ast::Node_id
    {
        // `\name. ...` shadows the variable we are replacing.
        if (abs.param == name) {
            return self;
        }

        // Alpha-renaming is only needed when the *replacement term* contains a
        // free occurrence of this binder's name. A bound occurrence inside the
        // replacement cannot be captured: it is already bound by its own lambda.
        //
        // Capture example: substituting x := y into \y. x y.
        //   replacement = y
        //   variable_occurs_free_in(ctx, "y", replacement) == true
        // so \y must be renamed; otherwise the inserted y would become bound.
        //
        // Safe example: substituting x := \y.y into \y. x y.
        //   replacement = \y.y
        //   variable_occurs_free_in(ctx, "y", replacement) == false
        // because the y in the replacement is bound by the replacement's own
        // lambda. Keeping \y unchanged captures nothing.
        //
        // This check is about `replacement`, not about `abs.body`. Seeing "y"
        // bound inside the body (for example in \x.\y. x y) says nothing about
        // whether the replacement needs renaming.
        if (!variable_occurs_free_in(ctx, abs.param, replacement)) {
            return ctx.make_abs(abs.param, (*this)(abs.body));
        }

        // Capture risk: choose a fresh name, rename the binder and its bound
        // occurrences, then substitute into the renamed body.
        //
        //     \y. x y  ->  \y'. x y'  ->  \y'. y y'
        auto fresh = fresh_name(abs.param, abs.body);
        if (renames != nullptr) {
            renames->emplace_back(abs.param, fresh);
        }
        auto renamed_body = Substitutor{ctx, abs.param, ctx.make_var(fresh), renames}(abs.body);
        return ctx.make_abs(fresh, (*this)(renamed_body));
    }

    // Application case: substitute in both sides and rebuild the node.
    auto apply(lc::ast::Node_id, const lc::ast::App &app) const -> lc::ast::Node_id
    {
        return ctx.make_app((*this)(app.left), (*this)(app.right));
    }

    // Placeholder nodes contain no variables.
    auto apply(lc::ast::Node_id self, const std::monostate &) const -> lc::ast::Node_id
    {
        return self;
    }

    // Pick a name that is not the variable being replaced, not free in the
    // replacement term, and not free in the body. The apostrophe suffix keeps
    // the name recognisable: y, y', y'', ...
    auto fresh_name(const std::string &base, lc::ast::Node_id body) const -> std::string
    {
        std::string candidate = base;
        while (candidate == name || variable_occurs_free_in(ctx, candidate, replacement) || variable_occurs_free_in(ctx, candidate, body)) {
            candidate += '\'';
        }
        return candidate;
    }
};
