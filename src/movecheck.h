#pragma once

#include <stdexcept>
#include <string>

#include "ast.h"

namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

void monomorphize_generics(Program& program);
void check_moves(const Program& program);

} // namespace scpp
