// ch05 §5.x: a *const* reference parameter binds directly to a fresh
// rvalue argument (a literal here) -- exactly like real C++'s own
// temporary materialization. A *mutable* `T&` still cannot (see
// mutable_reference_parameter_binding_to_a_literal_is_rejected.cpp).
int identity(const int& r) {
    return r;
}
int main() {
    return identity(5);
}
