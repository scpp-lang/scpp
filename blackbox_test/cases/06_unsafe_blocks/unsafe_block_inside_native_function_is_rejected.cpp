// ch01 §1.1/§1.3: "unsafe { } written inside a native function's body ...
// is a compile error: a native function has no active checking for
// either block to relax or re-enable."
void f() {
    unsafe {
        int x = 1;
    }
    return;
}

int main() {
    f();
    return 0;
}
