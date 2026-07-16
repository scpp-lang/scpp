module scpp.compiler.movecheck;

import :dataflow;
import :monomorphize;

namespace scpp {

void monomorphize_generics(Program& program) {
    monomorphize_generics_impl(program);
}

void check_moves(const Program& program) {
    check_moves_impl(program);
}

} // namespace scpp
