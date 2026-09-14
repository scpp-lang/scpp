module scpp.compiler.movecheck;

import std;
import :dataflow;
import :monomorphize;

namespace scpp {

static_assert(std::is_nothrow_move_constructible_v<Function>);
static_assert(std::is_nothrow_move_constructible_v<ClassDef>);
static_assert(std::is_nothrow_move_constructible_v<StructDef>);
static_assert(std::is_nothrow_move_constructible_v<EnumDef>);
static_assert(std::is_nothrow_move_constructible_v<ConceptDef>);
static_assert(std::is_nothrow_move_constructible_v<Stmt>);
static_assert(std::is_nothrow_move_constructible_v<Expr>);
static_assert(std::is_nothrow_move_constructible_v<Type>);
static_assert(std::is_nothrow_move_constructible_v<Param>);

std::expected<void, DataflowError> monomorphize_generics(Program& program) {
    return monomorphize_generics_impl(program);
}

std::expected<void, DataflowError> check_moves(const Program& program) {
    return check_moves_impl(program);
}

} // namespace scpp
