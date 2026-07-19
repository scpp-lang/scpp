module;

export module scpp.compiler.movecheck:errors;

import std;
import scpp.ast;

export namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

} // namespace scpp
