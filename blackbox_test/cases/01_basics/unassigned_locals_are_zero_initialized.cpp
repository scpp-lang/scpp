// ch05 §5.4 "Initialization": scpp has no concept of an uninitialized
// variable -- any local with no explicit initializer is guaranteed
// zero-initialized (0 for int, false for bool).
int main() {
    int x;
    bool b;
    if (b) {
        return 1;
    }
    return x;
}
