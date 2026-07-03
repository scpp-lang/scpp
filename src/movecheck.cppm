module;

#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

export module scpp.movecheck;

import scpp.ast;
import scpp.mir;

export namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message) : std::runtime_error(message) {}
};

} // namespace scpp

namespace scpp {
namespace {

// The lattice tracked per local variable:
//   Bottom      -- not currently in scope on this path: either not yet
//                  declared here (e.g. a name declared only in a sibling
//                  `if` branch), or already gone out of lexical scope (a
//                  `ScopeExit` MIR statement resets a local back to this
//                  state at the end of its enclosing block/if-branch/
//                  while-body, mirroring codegen's own scope_stack_).
//   Initialized -- holds a valid, readable value. scpp has no concept of an
//                  "uninitialized" scalar: every local without an explicit
//                  initializer is zero-initialized (0 / false / null / all-
//                  zero fields) by codegen, for every type, so a `Declare`
//                  MIR statement always yields Initialized, never a
//                  separate "not yet given a value" state.
//   MovedOut    -- was holding a unique_ptr value that has been moved away
//   Conflict    -- different incoming paths disagree (e.g. Initialized on
//                  one, MovedOut on another) -- treated as unsafe to read,
//                  same as Bottom/MovedOut.
// Only `Initialized` is safe to read; every other state is rejected (with a
// tailored message) if the checker sees a use of it.
enum class LocalState { Bottom, Initialized, MovedOut, Conflict };

using StateMap = std::unordered_map<std::string, LocalState>;

LocalState join(LocalState a, LocalState b) {
    if (a == b) return a;
    if (a == LocalState::Bottom) return b;
    if (b == LocalState::Bottom) return a;
    return LocalState::Conflict;
}

// Joins two per-block state snapshots (e.g. the OUT states of two
// predecessors flowing into a shared successor block).
StateMap join_maps(const StateMap& a, const StateMap& b) {
    StateMap result = a;
    for (const auto& [name, state] : b) {
        auto it = result.find(name);
        result[name] = it == result.end() ? state : join(it->second, state);
    }
    return result;
}

std::string describe_bad_state(const std::string& name, LocalState state) {
    switch (state) {
        case LocalState::MovedOut:
            return "use of moved-out variable '" + name + "'";
        case LocalState::Conflict:
            return "use of variable '" + name +
                   "' whose initialization state is inconsistent across incoming control-flow paths "
                   "(initialized on some, not on others)";
        case LocalState::Bottom:
            return "use of variable '" + name + "' that is out of scope here";
        case LocalState::Initialized:
        default:
            return "use of possibly-uninitialized variable '" + name + "'";
    }
}

[[nodiscard]] bool is_unique_ptr(const Type& type) { return type.kind == TypeKind::UniquePtr; }

// The only expressions allowed to produce a std::unique_ptr rvalue:
// moving an existing one out, or freshly heap-allocating one via
// std::make_unique (scpp has no `new` expression at all -- make_unique is
// the sole sanctioned allocation syntax, and is itself a compiler builtin
// rather than a real generic call, same treatment as std::move).
[[nodiscard]] bool produces_unique_ptr_rvalue(const Expr& expr) {
    return expr.kind == ExprKind::Move || expr.kind == ExprKind::MakeUnique;
}

[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name) {
    auto it = state.find(name);
    return it == state.end() ? LocalState::Bottom : it->second;
}

std::vector<size_t> successors(const Terminator& term) {
    switch (term.kind) {
        case TerminatorKind::Goto: return {term.target};
        case TerminatorKind::Branch: return {term.true_target, term.false_target};
        case TerminatorKind::Return:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
        default: return {};
    }
}

// Walks `expr`, updating `state` for any std::move / assignment side
// effects and, when `report_errors` is true, throwing DataflowError on an
// unsafe read. `is_unique_ptr_rvalue_context` is true exactly where a bare
// `std::move(x)` is allowed to appear: a var-decl initializer, an
// assignment RHS, a return value, or a call argument.
//
// This function is run twice per program point: once during the
// worklist's fixed-point iteration (report_errors=false, just to compute
// stable per-block states) and once more in the final reporting pass
// (report_errors=true). Both runs must apply the *same* state mutations so
// the two phases stay consistent.
void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, StateMap& state, const Body& body,
                 bool report_errors) {
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
            return;

        case ExprKind::Identifier: {
            if (!report_errors) return;
            auto type_it = body.local_types.find(expr.name);
            if (type_it == body.local_types.end()) return; // unknown name: left to codegen's own check
            if (is_unique_ptr(type_it->second)) {
                throw DataflowError("use of std::unique_ptr variable '" + expr.name +
                                     "' requires std::move (copying is not allowed)");
            }
            LocalState current = lookup(state, expr.name);
            if (current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(expr.name, current));
            }
            return;
        }

        case ExprKind::Move: {
            if (expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    throw DataflowError("std::move currently only supports a plain local variable "
                                         "(not a member, subscript, or other expression)");
                }
                return;
            }
            const std::string& name = expr.lhs->name;
            auto type_it = body.local_types.find(name);
            if (type_it == body.local_types.end() || !is_unique_ptr(type_it->second)) {
                if (report_errors) {
                    throw DataflowError("std::move is only supported for std::unique_ptr variables in this "
                                         "version; '" +
                                         name + "' is not one");
                }
                return;
            }
            LocalState current = lookup(state, name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(name, current));
            }
            state[name] = LocalState::MovedOut;
            if (report_errors && !is_unique_ptr_rvalue_context) {
                throw DataflowError("std::move(" + name + ") must be used to initialize or assign into a "
                                                            "std::unique_ptr");
            }
            return;
        }

        case ExprKind::Unary:
            apply_expr(*expr.lhs, false, state, body, report_errors);
            return;

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_unique_ptr = false;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    target_is_unique_ptr = it != body.local_types.end() && is_unique_ptr(it->second);
                }
                apply_expr(*expr.rhs, target_is_unique_ptr, state, body, report_errors);
                if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*expr.rhs)) {
                    throw DataflowError("assigning to a std::unique_ptr variable requires std::move or "
                                        "std::make_unique (copying is not allowed)");
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    // The assignment target is never a "read": whatever
                    // its previous state, assigning any value returns it
                    // to Initialized (spec ch05.1).
                    state[expr.lhs->name] = LocalState::Initialized;
                } else {
                    // e.g. `p.x = 1;` or `arr[i] = 1;`: the base
                    // object/index are evaluated (as addresses / an
                    // index value), not read as "the assignment target",
                    // so still worth walking for nested reads.
                    apply_expr(*expr.lhs, false, state, body, report_errors);
                }
                return;
            }
            apply_expr(*expr.lhs, false, state, body, report_errors);
            apply_expr(*expr.rhs, false, state, body, report_errors);
            return;

        case ExprKind::Call:
            for (const auto& arg : expr.args) {
                // Parameter types aren't threaded through here (the
                // checker doesn't resolve call targets to their declared
                // signatures); a unique_ptr argument must be
                // `std::move(x)` regardless of position, so std::move is
                // simply allowed in every argument slot.
                apply_expr(*arg, /*is_unique_ptr_rvalue_context=*/true, state, body, report_errors);
            }
            return;

        case ExprKind::Member:
            apply_expr(*expr.lhs, false, state, body, report_errors);
            return;

        case ExprKind::Subscript:
            apply_expr(*expr.lhs, false, state, body, report_errors);
            apply_expr(*expr.rhs, false, state, body, report_errors);
            return;

        case ExprKind::MakeUnique:
            // std::make_unique<T>(args...) itself never reads a
            // unique_ptr (it *produces* one); its constructor arguments
            // are ordinary values, checked as plain reads.
            for (const auto& arg : expr.args) {
                apply_expr(*arg, /*is_unique_ptr_rvalue_context=*/false, state, body, report_errors);
            }
            return;
    }
}

void apply_statement(const MirStatement& stmt, StateMap& state, const Body& body, bool report_errors) {
    switch (stmt.kind) {
        case MirStatementKind::Declare:
            // scpp has no "uninitialized" state (see the LocalState
            // comment above): a bare declaration always zero-initializes,
            // so it's always Initialized from this point on.
            state[stmt.local] = LocalState::Initialized;
            return;

        case MirStatementKind::Assign: {
            bool target_is_unique_ptr =
                body.local_types.contains(stmt.local) && is_unique_ptr(body.local_types.at(stmt.local));
            apply_expr(*stmt.expr, target_is_unique_ptr, state, body, report_errors);
            if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*stmt.expr)) {
                throw DataflowError("variable '" + stmt.local +
                                    "' of type std::unique_ptr must be initialized via std::move or "
                                    "std::make_unique (copying a unique_ptr is not allowed)");
            }
            state[stmt.local] = LocalState::Initialized;
            return;
        }

        case MirStatementKind::Eval:
            apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, report_errors);
            return;

        case MirStatementKind::Drop:
            // Purely a codegen-facing marker (no-op until heap-allocated
            // owning types exist); no dataflow state effect here.
            return;

        case MirStatementKind::ScopeExit:
            // `stmt.local` just went out of lexical scope: forget its
            // tracked state entirely. Erasing is equivalent to setting it
            // to Bottom (lookup() treats a missing key as Bottom) and
            // keeps the map from growing with entries the rest of the
            // analysis no longer cares about.
            state.erase(stmt.local);
            return;
    }
}

void check_terminator(const Terminator& term, StateMap& state, const Function& fn, const Body& body) {
    switch (term.kind) {
        case TerminatorKind::Branch:
            apply_expr(*term.condition, false, state, body, /*report_errors=*/true);
            return;
        case TerminatorKind::Return: {
            if (term.return_value == nullptr) return;
            bool return_is_unique_ptr = is_unique_ptr(fn.return_type);
            apply_expr(*term.return_value, return_is_unique_ptr, state, body, /*report_errors=*/true);
            if (return_is_unique_ptr && !produces_unique_ptr_rvalue(*term.return_value)) {
                throw DataflowError(
                    "returning a std::unique_ptr requires std::move or std::make_unique (copying is not allowed)");
            }
            return;
        }
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return;
    }
}

// Runs the worklist algorithm (see spec ch07/M3) to a fixed point over
// `body`'s CFG, computing a stable per-block entry ("IN") state for the
// definite-initialization/move lattice above, then makes one more pass
// reporting any unsafe use found using those now-stable states. Splitting
// into these two phases avoids both false positives (from not-yet-stable
// intermediate states) and duplicate diagnostics (a block can be visited
// many times during fixed-point iteration).
void check_function(const Function& fn) {
    Body body = build_mir(fn);
    size_t n = body.blocks.size();

    std::vector<std::vector<size_t>> preds(n);
    for (size_t i = 0; i < n; i++) {
        for (size_t succ : successors(body.blocks[i].terminator)) {
            preds[succ].push_back(i);
        }
    }

    StateMap entry_state;
    for (const Param& param : fn.params) {
        entry_state[param.name] = LocalState::Initialized;
    }

    std::vector<StateMap> in_states(n);
    std::vector<StateMap> out_states(n);
    if (n > 0) in_states[0] = entry_state;

    std::deque<size_t> worklist;
    std::vector<bool> queued(n, false);
    for (size_t i = 0; i < n; i++) {
        worklist.push_back(i);
        queued[i] = true;
    }

    while (!worklist.empty()) {
        size_t b = worklist.front();
        worklist.pop_front();
        queued[b] = false;

        StateMap new_in;
        if (b == 0) {
            new_in = entry_state;
        } else {
            bool first = true;
            for (size_t p : preds[b]) {
                new_in = first ? out_states[p] : join_maps(new_in, out_states[p]);
                first = false;
            }
        }

        StateMap new_out = new_in;
        for (const MirStatement& stmt : body.blocks[b].statements) {
            apply_statement(stmt, new_out, body, /*report_errors=*/false);
        }

        in_states[b] = new_in;
        bool out_changed = new_out != out_states[b];
        out_states[b] = std::move(new_out);

        if (out_changed) {
            for (size_t succ : successors(body.blocks[b].terminator)) {
                if (!queued[succ]) {
                    worklist.push_back(succ);
                    queued[succ] = true;
                }
            }
        }
    }

    // Fixed point reached: `in_states` is now stable. Walk every block
    // once more, this time actually reporting diagnostics.
    for (size_t b = 0; b < n; b++) {
        StateMap state = in_states[b];
        for (const MirStatement& stmt : body.blocks[b].statements) {
            apply_statement(stmt, state, body, /*report_errors=*/true);
        }
        check_terminator(body.blocks[b].terminator, state, fn, body);
    }
}

} // namespace
} // namespace scpp

export namespace scpp {

void check_moves(const Program& program) {
    for (const Function& fn : program.functions) {
        check_function(fn);
    }
}

} // namespace scpp
