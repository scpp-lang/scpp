module;

#include <stdexcept>
#include <string>
#include <unordered_map>

export module scpp.movecheck;

import scpp.ast;

export namespace scpp {

struct MoveError : std::runtime_error {
    explicit MoveError(const std::string& message) : std::runtime_error(message) {}
};

// A minimal, function-local move-out checker: the "simplest sound check"
// called for by M2 (spec ch05.1), implemented as a direct walk of the AST
// rather than the MIR-based dataflow analysis planned for M3+. Only
// `std::unique_ptr<T>` locals/params are tracked; every other type is
// Copy (scalars) or not yet move-checked (structs, per ch04, never need
// tracking since they're always trivially copyable).
//
// Rules enforced:
//   - A unique_ptr value can only be produced by `std::move(x)` (moving `x`
//     out) or by a fresh declaration with no initializer (an empty/null
//     unique_ptr, mirroring the real default constructor).
//   - Reading a unique_ptr variable any other way (bare use in an
//     expression, passed by value without `std::move`, etc.) is rejected --
//     real C++ has no copy constructor for unique_ptr either.
//   - `std::move(x)` requires `x` to currently be Initialized; using
//     `std::move` on an already-moved-out variable is an error.
//   - Assigning any value into a variable (via `=` or through
//     std::move(...)) returns it to the Initialized state, regardless of
//     its previous state.
//   - `if`/`while` branches are checked independently from the same
//     pre-branch state; the state after is merged conservatively (a
//     variable moved in *either* branch is treated as moved-out
//     afterwards). This is a sound but imprecise approximation of full
//     dataflow analysis, acceptable for M2's "simplest check" scope.
class MoveChecker {
public:
    void check(const Program& program) {
        for (const Function& fn : program.functions) {
            check_function(fn);
        }
    }

private:
    enum class State { Initialized, MovedOut };

    std::unordered_map<std::string, State> states_;
    std::unordered_map<std::string, Type> types_;

    [[nodiscard]] static bool is_unique_ptr(const Type& type) { return type.kind == TypeKind::UniquePtr; }

    void check_function(const Function& fn) {
        states_.clear();
        types_.clear();
        for (const Param& param : fn.params) {
            if (is_unique_ptr(param.type)) {
                states_[param.name] = State::Initialized;
                types_[param.name] = param.type;
            }
        }
        check_stmt(*fn.body, fn.return_type);
    }

    // Merges two post-branch state maps conservatively: a unique_ptr
    // variable is MovedOut after the merge if it was moved in *either*
    // branch (see class comment).
    static std::unordered_map<std::string, State> merge_states(
        const std::unordered_map<std::string, State>& a, const std::unordered_map<std::string, State>& b) {
        std::unordered_map<std::string, State> merged = a;
        for (const auto& [name, state] : b) {
            if (state == State::MovedOut) merged[name] = State::MovedOut;
            else if (!merged.contains(name)) merged[name] = state;
        }
        return merged;
    }

    void check_stmt(const Stmt& stmt, const Type& current_function_return_type) {
        switch (stmt.kind) {
            case StmtKind::Block:
                for (const auto& s : stmt.statements) {
                    check_stmt(*s, current_function_return_type);
                }
                return;

            case StmtKind::VarDecl: {
                if (stmt.init) {
                    check_expr(*stmt.init, /*is_unique_ptr_rvalue_context=*/is_unique_ptr(stmt.type));
                }
                if (is_unique_ptr(stmt.type)) {
                    if (stmt.init && stmt.init->kind != ExprKind::Move) {
                        throw MoveError("variable '" + stmt.var_name +
                                         "' of type std::unique_ptr must be initialized via std::move "
                                         "(copying a unique_ptr is not allowed)");
                    }
                    states_[stmt.var_name] = State::Initialized;
                    types_[stmt.var_name] = stmt.type;
                }
                return;
            }

            case StmtKind::Return: {
                if (stmt.expr) {
                    bool return_is_unique_ptr = is_unique_ptr(current_function_return_type);
                    check_expr(*stmt.expr, return_is_unique_ptr);
                    if (return_is_unique_ptr && stmt.expr->kind != ExprKind::Move) {
                        throw MoveError("returning a std::unique_ptr requires std::move (copying is not allowed)");
                    }
                }
                return;
            }

            case StmtKind::ExprStmt:
                check_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false);
                return;

            case StmtKind::If: {
                check_expr(*stmt.condition, /*is_unique_ptr_rvalue_context=*/false);
                auto pre_states = states_;

                check_stmt(*stmt.then_branch, current_function_return_type);
                auto then_states = states_;

                states_ = pre_states;
                if (stmt.else_branch) {
                    check_stmt(*stmt.else_branch, current_function_return_type);
                }
                auto else_states = states_;

                states_ = merge_states(then_states, else_states);
                return;
            }

            case StmtKind::While: {
                check_expr(*stmt.condition, /*is_unique_ptr_rvalue_context=*/false);
                auto pre_states = states_;

                check_stmt(*stmt.then_branch, current_function_return_type);

                // The loop may execute zero or more times; conservatively
                // merge "didn't run" (pre_states) with "ran the body once"
                // (states_ after one pass) rather than computing a true
                // fixed point -- sound but imprecise, matching M2's scope.
                states_ = merge_states(pre_states, states_);
                return;
            }
        }
    }

    // `is_unique_ptr_rvalue_context` is true when this expression's *value*
    // will be stored into a unique_ptr-typed place (a var decl initializer,
    // an assignment RHS, a return value, or a unique_ptr-typed call
    // argument): in that position the expression must be exactly
    // `std::move(x)`. Elsewhere, a bare reference to a unique_ptr variable
    // is rejected outright (see class comment).
    void check_expr(const Expr& expr, bool is_unique_ptr_rvalue_context) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral:
            case ExprKind::BoolLiteral:
                return;

            case ExprKind::Identifier: {
                auto it = types_.find(expr.name);
                if (it != types_.end() && is_unique_ptr(it->second)) {
                    throw MoveError("use of std::unique_ptr variable '" + expr.name +
                                     "' requires std::move (copying is not allowed)");
                }
                return;
            }

            case ExprKind::Move: {
                if (expr.lhs->kind != ExprKind::Identifier) {
                    throw MoveError("std::move currently only supports a plain local variable "
                                     "(not a member, subscript, or other expression)");
                }
                const std::string& name = expr.lhs->name;
                auto type_it = types_.find(name);
                if (type_it == types_.end() || !is_unique_ptr(type_it->second)) {
                    throw MoveError("std::move is only supported for std::unique_ptr variables in this "
                                     "version; '" +
                                     name + "' is not one");
                }
                auto state_it = states_.find(name);
                if (state_it != states_.end() && state_it->second == State::MovedOut) {
                    throw MoveError("use of moved-out variable '" + name + "'");
                }
                states_[name] = State::MovedOut;
                if (!is_unique_ptr_rvalue_context) {
                    throw MoveError("std::move(" + name +
                                     ") must be used to initialize or assign into a std::unique_ptr");
                }
                return;
            }

            case ExprKind::Unary:
                check_expr(*expr.lhs, /*is_unique_ptr_rvalue_context=*/false);
                return;

            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Assign) {
                    // The assignment target is never a "read": moving into
                    // it always returns it to Initialized, regardless of
                    // its current state, so it is intentionally not
                    // checked via the generic Identifier-read path.
                    bool target_is_unique_ptr = false;
                    if (expr.lhs->kind == ExprKind::Identifier) {
                        auto it = types_.find(expr.lhs->name);
                        target_is_unique_ptr = it != types_.end() && is_unique_ptr(it->second);
                    }
                    check_expr(*expr.rhs, target_is_unique_ptr);
                    if (target_is_unique_ptr && expr.rhs->kind != ExprKind::Move) {
                        throw MoveError("assigning to a std::unique_ptr variable requires std::move "
                                        "(copying is not allowed)");
                    }
                    if (expr.lhs->kind == ExprKind::Identifier && target_is_unique_ptr) {
                        states_[expr.lhs->name] = State::Initialized;
                    } else {
                        check_expr(*expr.lhs, /*is_unique_ptr_rvalue_context=*/false);
                    }
                    return;
                }
                check_expr(*expr.lhs, /*is_unique_ptr_rvalue_context=*/false);
                check_expr(*expr.rhs, /*is_unique_ptr_rvalue_context=*/false);
                return;

            case ExprKind::Call:
                for (const auto& arg : expr.args) {
                    // Parameter types aren't threaded through here (the
                    // checker doesn't currently resolve call targets to
                    // their declared signatures); a unique_ptr argument
                    // must be `std::move(x)` regardless of position, so we
                    // simply allow std::move in every argument slot.
                    check_expr(*arg, /*is_unique_ptr_rvalue_context=*/true);
                }
                return;

            case ExprKind::Member:
                check_expr(*expr.lhs, /*is_unique_ptr_rvalue_context=*/false);
                return;

            case ExprKind::Subscript:
                check_expr(*expr.lhs, /*is_unique_ptr_rvalue_context=*/false);
                check_expr(*expr.rhs, /*is_unique_ptr_rvalue_context=*/false);
                return;
        }
    }
};

void check_moves(const Program& program) {
    MoveChecker checker;
    checker.check(program);
}

} // namespace scpp
