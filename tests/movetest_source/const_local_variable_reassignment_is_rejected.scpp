// ch05/ch06: a `const`-qualified local is initialized exactly once, by
// its own declaration -- any later reassignment is rejected, distinct
// from an ordinary (mutable) local, which stays freely reassignable
// throughout its scope.
int main() {
    const int x = 5;
    x = 10;
    return x;
}
