// ch05 §5.8: "In ... an entirely unsafe function (the default,
// unannotated context): the check is skipped" the same way as inside
// unsafe { }. `f` here has no `safe` annotation at all.
int f(int a, int b) {
    return a + b;
}

// INT_MAX + 2 wraps to INT_MIN + 1 == -2147483647 (0x80000001); low byte
// (exit code) is 1.
int main() {
    return f(2147483647, 2);
}
