// ch03: `T&` is a mutable/exclusive borrow -- writing through it is
// visible through the original variable name too (they're the same
// storage).
safe int f() {
    int x = 5;
    int& r = x;
    r = 10;
    return x;
}

int main() {
    return f();
}
