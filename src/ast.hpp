#pragma once

// The AST is stored in an append-only arena (`Ast_context`). A node is
// identified by a small integer (`Node_id`) rather than a pointer, so copying a
// node id is cheap and remains valid even when the arena grows.
//
// For example, the source term
//
//     (\x.\y. x y) y
//
// is represented roughly as:
//
//     App(
//       Abs("x",
//         Abs("y",
//           App(Var("x"), Var("y")))),
//       Var("y"))
//
// The final `y` is the free outer variable. The `y` inside the body is bound by
// the inner `\y`, so those two `y`s are different variables in spirit even
// though they are spelled the same. The substitution code is responsible for
// keeping them apart with alpha-renaming.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lc {

// A strongly typed wrapper around an integer id.
//
// `Strong_id<Tag>` is a distinct type for every tag, so ids for different kinds
// of objects cannot be mixed up accidentally. It cannot be constructed from, or
// implicitly converted to, its underlying integer type; dereference it with
// `*id` to opt in.
//
// Example:
//
//     struct Node_tag;
//     using Node_id = Strong_id<Node_tag>;
//     Node_id id{42};
//     auto raw = *id;             // 42
//     auto raw2 = static_cast<std::uint32_t>(id); // also 42
template <typename Tag_type>
class Strong_id
{
public:
    using Tag = Tag_type;
    using Value_type = std::uint32_t;

    // Force callers to spell out the type: `Node_id{0}`, not `Node_id id = 0`.
    Strong_id() = delete;

    explicit constexpr Strong_id(Value_type value) noexcept : _value(value) {}

    // Dereference-style access to the raw id.
    [[nodiscard]] constexpr auto operator*() const noexcept -> Value_type
    {
        return _value;
    }

    // Explicit conversion. `static_cast<Value_type>(id)` works, but implicit
    // conversion to an integer does not.
    [[nodiscard]] constexpr explicit operator Value_type() const noexcept
    {
        return _value;
    }

    friend constexpr auto operator==(Strong_id, Strong_id) noexcept -> bool = default;
    friend constexpr auto operator<=>(Strong_id, Strong_id) noexcept = default;

private:
    Value_type _value;
};

} // namespace lc

namespace lc::ast {

// Tag type used to create the distinct `Node_id` type below.
struct Node_tag;
using Node_id = Strong_id<Node_tag>;

// These compile-time checks document and enforce the properties of Node_id:
// it is not an integer, it is not implicitly convertible in either direction,
// and its raw value can still be recovered explicitly.
static_assert(!std::is_convertible_v<std::uint32_t, Node_id>);
static_assert(!std::is_convertible_v<Node_id, std::uint32_t>);
static_assert(std::is_constructible_v<Node_id::Value_type, Node_id>);
static_assert([] {
    const Node_id id{42};
    return *id == 42U && static_cast<Node_id::Value_type>(id) == 42U;
}());

// A variable occurrence, for example `x` in `\y. x y`.
struct Var
{
    std::string name;
};

// A lambda abstraction, for example `\x. x y`.
// `param` is the bound name and `body` points at the body node.
struct Abs
{
    std::string param;
    Node_id body;
};

// A function application, for example `x y`.
// `left` is the function and `right` is the argument.
struct App
{
    Node_id left;
    Node_id right;
};

// One AST node. `std::monostate` is used as an empty/placeholder node when one
// is needed; it prints as `<\0>`.
using Node_data = std::variant<Var, Abs, App, std::monostate>;

// Owns every AST node created while parsing or evaluating.
//
// Nodes are never mutated in place. Evaluators append new nodes and return their
// ids, which makes it easy to keep old terms around for tracing. For example,
// evaluating `(\x.x) y` can create the result node for `y` while the original
// application node still exists in the arena.
struct Ast_context
{
    std::pmr::vector<Node_data> memory;

    // Reserve space up front to avoid frequent reallocations. This is only a
    // performance hint; the vector still grows if more nodes are needed.
    explicit Ast_context(std::pmr::memory_resource *mr) : memory(mr)
    {
        memory.reserve(100000); // Prevent frequent vector reallocations
    }

    // Each make_* function appends exactly one node and returns the id of that
    // new node. The returned id is stable for the lifetime of the context.
    auto make_var(std::string name) -> Node_id
    {
        memory.push_back(Var{std::move(name)});
        return current_id();
    }

    auto make_abs(std::string param, Node_id body) -> Node_id
    {
        memory.push_back(Abs{std::move(param), body});
        return current_id();
    }

    auto make_app(Node_id left, Node_id right) -> Node_id
    {
        memory.push_back(App{left, right});
        return current_id();
    }

    auto make_mono() -> Node_id
    {
        memory.push_back(std::monostate{});
        return current_id();
    }

    // Returns the node for an id. The const overload is used by readers such as
    // the pretty-printer and `is_variable_free`.
    auto get(Node_id id) -> Node_data &
    {
        return memory[checked_index(id)];
    }
    auto get(Node_id id) const -> const Node_data &
    {
        return memory[checked_index(id)];
    }

private:
    // The id of the node that was just appended. `memory.size() - 1` is the
    // index of the last element.
    auto current_id() const -> Node_id
    {
        if (memory.size() > std::numeric_limits<Node_id::Value_type>::max()) [[unlikely]] {
            throw std::length_error("AST exceeds the maximum number of nodes");
        }
        return Node_id{static_cast<Node_id::Value_type>(memory.size() - 1)};
    }

    // Converts a Node_id to a vector index and validates it. This catches stale
    // or fabricated ids instead of indexing out of bounds.
    auto checked_index(Node_id id) const -> std::size_t
    {
        auto index = static_cast<std::size_t>(*id);
        if (index >= memory.size()) [[unlikely]] {
            throw std::out_of_range(std::format("invalid AST node id: {}", *id));
        }
        return index;
    }
};

} // namespace lc::ast
