// ch05 §5.11: "A concept is optional, not mandatory, on a constrained
// parameter. Writing bare auto (no concept name in front of it) is
// legal -- it means the parameter's type is treated as fully opaque."
// A free function's bare `auto` parameter parses, is monomorphized per
// call site (reusing the abbreviated-Concept-auto-form's own witness/
// substitution machinery against a reserved, empty-requirements "$auto"
// witness), and compiles/runs correctly for a plain scalar argument.
int identity(auto x) {
    return x;
}
int main() {
    return identity(42);
}
