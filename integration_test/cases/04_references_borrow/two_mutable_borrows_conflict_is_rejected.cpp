// ch05 §5.2: "at any instant an object may have ... exactly one T&
// (mutable borrow), never" two.
safe int f() {
    int x = 5;
    int& r1 = x;
    int& r2 = x;
    return r1 + r2;
}

int main() {
    return f();
}
