#pragma once

// Named definitions ("environment") for lambda terms.
//
// Definitions let a program give a name to a closed term:
//
//     identity = \a.a
//
// and then use that name anywhere a variable can appear:
//
//     identity identity
//
// A definition is stored as an ordinary AST term. Before a term is evaluated,
// free occurrences of defined names are replaced with their definitions. This
// is a macro-style expansion, not a new evaluation mechanism, so both evaluator
// strategies keep working unchanged.
//
// Definitions are required to be closed: every variable in the definition must
// either be bound by a lambda inside the definition or be another definition.
// Rejecting free variables is what makes expansion safe. Because every stored
// definition is closed, inserting it underneath, say, `\y. ...` can never
// capture a `y` from the use site:
//
//     identity = \a.a;
//     \identity. identity          # the binder wins; nothing is expanded
//
// Definitions are not recursive. Use a fixed-point combinator when recursion is
// needed; a name cannot refer to itself while it is being defined.

#include "ast.hpp"

#include <algorithm>
#include <map>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lc {

// Maps a definition name to the AST of its (expanded, closed) value.
struct Environment
{
    std::pmr::map<std::string, ast::Node_id> values;

    explicit Environment(std::pmr::memory_resource *mr) : values(mr) {}
};

// Result of expanding definitions in a term.
struct Expansion
{
    // The term with all known definitions substituted.
    ast::Node_id node;

    // Free names that were not defined. Expressions may keep these (they are
    // ordinary free variables), but definitions are rejected when this is
    // non-empty.
    std::vector<std::string> unresolved;
};

namespace detail {

// Walks a term, keeping track of lambda-bound names so a global definition is
// only substituted for genuinely free occurrences.
class Expander
{
public:
    Expander(ast::Ast_context &ctx, const Environment &env) : _ctx(ctx), _env(env) {}

    auto operator()(ast::Node_id node) -> ast::Node_id
    {
        // Copy before recursing: expansion appends nodes to the arena, which may
        // reallocate its vector and invalidate references returned by `get`.
        auto data = _ctx.get(node);
        return std::visit([&](const auto &value) -> ast::Node_id { return apply(node, value); }, data);
    }

    auto take_unresolved() -> std::vector<std::string>
    {
        return std::move(_unresolved);
    }

private:
    ast::Ast_context &_ctx;
    const Environment &_env;
    std::vector<std::string> _bound;
    std::vector<std::string> _unresolved;

    auto is_bound(const std::string &name) const -> bool
    {
        return std::find(_bound.begin(), _bound.end(), name) != _bound.end();
    }

    // Free variable: replace it with its definition when one exists. A bound
    // occurrence is left alone, even if a global with the same name exists.
    auto apply(ast::Node_id self, const ast::Var &var) -> ast::Node_id
    {
        if (is_bound(var.name)) {
            return self;
        }

        if (auto it = _env.values.find(var.name); it != _env.values.end()) {
            return it->second;
        }

        _unresolved.push_back(var.name);
        return self;
    }

    // Expand the body under the binder, then rebuild the abstraction.
    auto apply(ast::Node_id, const ast::Abs &abs) -> ast::Node_id
    {
        _bound.push_back(abs.param);
        auto body = (*this)(abs.body);
        _bound.pop_back();
        return _ctx.make_abs(abs.param, body);
    }

    // Expand both sides of an application.
    auto apply(ast::Node_id, const ast::App &app) -> ast::Node_id
    {
        // Copy the ids before recursing: appending nodes can invalidate `app`.
        auto left = app.left;
        auto right = app.right;
        return _ctx.make_app((*this)(left), (*this)(right));
    }

    // Placeholder nodes contain no variables.
    auto apply(ast::Node_id self, const std::monostate &) -> ast::Node_id
    {
        return self;
    }
};

} // namespace detail

// Replace free occurrences of defined names in `node` with their definitions.
// Unresolved free names are reported in `Expansion::unresolved`.
inline auto expand_globals(ast::Ast_context &ctx, const Environment &env, ast::Node_id node) -> Expansion
{
    detail::Expander expander{ctx, env};
    auto expanded = expander(node);
    return {expanded, expander.take_unresolved()};
}

} // namespace lc
