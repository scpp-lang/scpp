module scpp.compiler.movecheck;

import std;
import :dataflow;
import :monomorphize;

namespace scpp {

std::expected<void, DataflowError> monomorphize_generics(Program& program) {
    return monomorphize_generics_impl(program);
}

std::expected<void, DataflowError> check_moves(const Program& program) {
    return check_moves_impl(program);
}

} // namespace scpp
