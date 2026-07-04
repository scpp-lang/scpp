// ch01 §1.3 / ch02: wrapping the call to a non-`safe` function in
// `unsafe { }` is exactly the "programmer vouches for it" escape hatch.
int helper() {
    return 7;
}

safe int caller() {
    int result = 0;
    unsafe {
        result = helper();
    }
    return result;
}

int main() {
    return caller();
}
