// ch05 §5.2: a shared borrow and a mutable borrow of the same object can
// never coexist.
safe int f() {
    int x = 5;
    const int& r1 = x;
    int& r2 = x;
    return r1 + r2;
}

int main() {
    return f();
}
