// ch03/ch08 Q1: the bounds check is "skipped inside unsafe {}/a native
// function instead, same treatment as integer-overflow checking." `main`
// here is an ordinary native function (no `safe` annotation), so an
// out-of-bounds `s[10]` must not abort -- the read value itself is
// unspecified (uninitialized stack garbage), so this only asserts the
// process terminates normally instead of via abort()'s SIGABRT.
int main() {
    int arr[3];
    std::span<int> s = arr;
    return s[10];
}
