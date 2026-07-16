module;

#include <stdexcept>
#include <string>

export module scpp.compiler.movecheck:errors;

import scpp.ast;

export namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

} // namespace scpp
