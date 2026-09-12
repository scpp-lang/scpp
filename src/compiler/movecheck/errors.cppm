module;

export module scpp.compiler.movecheck:errors;

import std;
import scpp.ast;

export namespace scpp {

class DataflowError : public std::runtime_error {
public:
    explicit DataflowError(const std::string& message, SourceLocation loc = {})
        : runtime_error{message}, loc{loc} {}

    DataflowError(const DataflowError& other)
        : runtime_error{std::string{other.what()}}, loc{other.loc} {}

    virtual ~DataflowError() override = default;

    SourceLocation loc{};
};

} // namespace scpp
