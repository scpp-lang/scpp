// ch01 §1.3: "unsafe { } may nest inside another unsafe { } -- harmless,
// not an error, as long as both are still lexically inside a safe
// function."
safe int f(int* p) {
    unsafe {
        unsafe {
            return *p;
        }
    }
}

int main() {
    int x = 9;
    return f(&x);
}
