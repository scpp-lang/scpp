module;

export module scpp.compiler.movecheck;

import std;
import scpp.ast;
export import :errors;

export namespace scpp {

[[nodiscard]] extern std::expected<void, DataflowError> monomorphize_generics(Program& program);
[[nodiscard]] extern std::expected<void, DataflowError> check_moves(const Program& program);

} // namespace scpp
