// ch05 §5.x: `const T& r = <literal>;` (a local reference binding, not a
// call argument -- see const_reference_parameter_binding_to_a_literal_is_allowed.cpp
// for the argument-passing shape) also binds directly to a fresh
// temporary.
int main() {
    const int& r = 5;
    return r;
}
