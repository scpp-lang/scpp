// Same as span_bounds_check_skipped_in_native_function, but the enclosing
// function is `safe` and only the access itself is inside `unsafe { }` --
// per ch01 §1.3, the bounds check is relaxed for the duration of the
// block, not just for an entirely-native function.
safe int oob(std::span<int> s) {
    unsafe {
        return s[10];
    }
}

int main() {
    int arr[3];
    std::span<int> s = arr;
    return oob(s);
}
