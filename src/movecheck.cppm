module;

export module scpp.compiler.movecheck;

import scpp.ast;
export import :errors;

export namespace scpp {

void monomorphize_generics(Program& program);
void check_moves(const Program& program);

} // namespace scpp
