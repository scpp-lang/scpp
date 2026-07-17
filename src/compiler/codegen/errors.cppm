module;

#include <stdexcept>
#include <string>

export module scpp.compiler.codegen:errors;

import scpp.ast;

export namespace scpp {

struct CodegenError : std::runtime_error {
    explicit CodegenError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

} // namespace scpp
