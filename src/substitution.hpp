#pragma once

// Free-variable checking and capture-avoiding substitution.
//
// These are the two operations the evaluators use to perform beta reduction:
//
//     (\x. body) value   ->   body[x := value]
//
// The tricky part is that names can be shadowed and substitution can capture a
// free variable. For example:
//
//     (\x.\y. x y) y
//
// Substituting `x := y` into `\y. x y` directly would give `\y. y y`, which is
// wrong: the argument `y` would become bound by the inner `\y`. Instead we
// rename the bound `y` to a fresh name first:
//
//     \y. x y   ->   \y'. x y'   ->   \y'. y y'
//
// This file implements that renaming and substitution.

#include "ast.hpp"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Returns true when `name` occurs free in `tree_to_search`.
//
// A variable is free when it is not bound by an enclosing lambda. Examples:
//
//     is_variable_free(ctx, "y", \x.\y. x y)  -> false  (bound by inner \y)
//     is_variable_free(ctx, "x", \x.\y. x y)  -> false  (bound by outer \x)
//     is_variable_free(ctx, "z", \x.\y. x y)  -> true   (z is not present)
//
// The App case checks both sides; the Abs case stops at the binder that matches
// the name, because occurrences under that binder refer to the bound variable.
inline auto is_variable_free(lc::ast::Ast_context &ctx, const std::string &name, lc::ast::Node_id tree_to_search) -> bool
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
                return search_space.param != name && is_variable_free(ctx, name, search_space.body);
            } else if constexpr (std::is_same_v<T, App>) {
                return is_variable_free(ctx, name, search_space.left) || is_variable_free(ctx, name, search_space.right);
            } else {
                return false;
            }
        },
        ctx.get(tree_to_search)); // Safe: is_variable_free doesn't allocate
}

// Capture-avoiding substitution: replace free occurrences of `name` in an AST
// with `value`.
//
// Example:
//
//     Substitutor{ctx, "x", ctx.make_var("y")}(body_of(\x.\y. x y))
//
// produces the term `\y'. y y'`: the inner binder `y` is renamed because the
// replacement value `y` would otherwise be captured.
//
// Shadowing example:
//
//     Substitutor{ctx, "x", value}(body_of(\x. x))  ->  \x. x
//
// The inner `\x` shadows the outer `x`, so the bound occurrence is left alone.
struct Substitutor
{
    lc::ast::Ast_context &ctx;
    std::string name;
    lc::ast::Node_id value;

    // Optional output parameter used by the trace renderer. Whenever an
    // alpha-rename happens, `(old_name, fresh_name)` is appended so the trace
    // can show annotations such as `(α: y → y')`.
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
        return v.name == name ? value : self;
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

        // No capture risk: the binder name does not occur free in the value.
        if (!is_variable_free(ctx, abs.param, value)) {
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
    // replacement value, and not free in the body. The apostrophe suffix keeps
    // the name recognisable: y, y', y'', ...
    auto fresh_name(const std::string &base, lc::ast::Node_id body) const -> std::string
    {
        std::string candidate = base;
        while (candidate == name || is_variable_free(ctx, candidate, value) || is_variable_free(ctx, candidate, body)) {
            candidate += '\'';
        }
        return candidate;
    }
};
