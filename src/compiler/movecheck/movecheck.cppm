module;

export module scpp.compiler.movecheck;

import std;
import scpp.ast;
export import :errors;

namespace scpp {

[[nodiscard]] extern std::expected<void, DataflowError> monomorphize_generics_impl(Program& program);
[[nodiscard]] extern std::expected<void, DataflowError> check_moves_impl(const Program& program);

} // namespace scpp

export namespace scpp {

[[nodiscard]] std::expected<void, DataflowError> monomorphize_generics(Program& program) {
    return monomorphize_generics_impl(program);
}

[[nodiscard]] std::expected<void, DataflowError> check_moves(const Program& program) {
    return check_moves_impl(program);
}

} // namespace scpp
