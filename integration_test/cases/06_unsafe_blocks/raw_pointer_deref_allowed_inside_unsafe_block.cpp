// ch01 §1.3: `unsafe { }` locally relaxes raw pointer dereference inside
// a `safe` function.
safe int f(int* p) {
    unsafe {
        return *p;
    }
}

int main() {
    int x = 5;
    return f(&x);
}
