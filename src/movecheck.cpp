#include "movecheck.h"

#include "compiler/movecheck/dataflow.h"
#include "compiler/movecheck/monomorphize.h"

namespace scpp {

void monomorphize_generics(Program& program) {
    monomorphize_generics_impl(program);
}

void check_moves(const Program& program) {
    check_moves_impl(program);
}

} // namespace scpp
