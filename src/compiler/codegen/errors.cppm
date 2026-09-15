module;

export module scpp.compiler.codegen:errors;

import std;
import scpp.ast;

export namespace scpp {

class CodegenError : public std::runtime_error {
public:
    explicit CodegenError(const std::string& message, SourceLocation loc = {})
        : runtime_error{message}, loc{loc} {}

    CodegenError(const CodegenError& other)
        : runtime_error{std::string{other.what()}}, loc{other.loc} {}

    virtual ~CodegenError() override = default;

    SourceLocation loc{};
};

} // namespace scpp
