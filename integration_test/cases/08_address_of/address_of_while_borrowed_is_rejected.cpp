// ch05 §5.7: "What is checked at the moment &expr is evaluated: ... the
// root must have no existing borrow at all (shared or mutable) at this
// instant, or &expr is rejected the same way taking a second T& would
// be." `x` already has a live mutable borrow (`r`) when `&x` is
// evaluated.
safe int f() {
    int x = 5;
    int& r = x;
    int* p = &x;
    return r;
}

int main() {
    return f();
}
