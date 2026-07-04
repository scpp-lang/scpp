// ch05 §5.7/§8 Q9: "Writing through a const T* is an ordinary compile-time
// type error, in every context, including inside unsafe { }." This is
// deliberately wrapped in `unsafe { }` to prove the rejection is *not*
// about the raw-pointer-deref gate (which unsafe{} would satisfy) but an
// unconditional type error that unsafe{} never relaxes.
safe int f() {
    int x = 5;
    const int* p = &x;
    unsafe {
        *p = 10;
    }
    return x;
}

int main() {
    return f();
}
