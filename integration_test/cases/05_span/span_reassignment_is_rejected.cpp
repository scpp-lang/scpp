// ch03: span "cannot be reassigned after construction (conservatively
// treated like a reference for now: bound once, never rebound)".
safe int f() {
    int arr1[2];
    int arr2[2];
    std::span<int> s = arr1;
    s = arr2;
    return 0;
}

int main() {
    return f();
}
