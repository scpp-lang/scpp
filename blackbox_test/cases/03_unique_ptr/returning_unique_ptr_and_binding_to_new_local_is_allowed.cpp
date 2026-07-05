// A function returning std::unique_ptr<T> by value, whose result is
// bound directly to a new named local at the call site -- the ordinary
// "factory function" pattern (`T x = factory();` is unremarkable,
// idiomatic C++, and ch00 principle 1 says scpp should look/feel exactly
// like it). ch09 M2 documents unique_ptr's "move semantics fit
// naturally", so this composition of two already-implemented features
// (functions returning std::unique_ptr<T>, and initializing a local from
// an expression) is expected to work.
std::unique_ptr<int> make_it() {
    return std::make_unique<int>(11);
}

int main() {
    std::unique_ptr<int> p = make_it();
    return *p;
}
