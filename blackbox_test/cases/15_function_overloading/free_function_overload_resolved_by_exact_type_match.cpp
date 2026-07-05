// ch05 §5.10: "scpp allows multiple functions (free functions or methods)
// to share a name, distinguished by parameter list only ... Resolution
// rule: exact type match only." f(int) and f(bool) share a name; each
// call resolves to whichever overload's parameter type exactly matches
// the argument's type.
safe int f(int x) {
    return x;
}

safe int f(bool b) {
    if (b) {
        return 100;
    }
    return 0;
}

// f(5) -> 5 (matches f(int)); f(true) -> 100 (matches f(bool)).
safe int g() {
    return f(5) + f(true);
}

int main() {
    return g();
}
