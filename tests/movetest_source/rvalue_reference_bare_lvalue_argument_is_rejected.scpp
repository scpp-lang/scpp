// ch03/ch05 §5.11: a bare named lvalue is never a legitimate argument for
// a `T&&` (rvalue-reference/move) parameter -- passing it directly
// (without an explicit std::move) would be exactly the unmarked implicit
// move ch05 §5.1 forbids.
int take(int&& x) { return x; }
int main() {
    int y = 5;
    return take(y);
}
