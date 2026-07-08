int access_unsafely(std::span<int> s, int i) {
    [[scpp::unsafe]] {
        return s[i];
    }
}
int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    // Deliberately out of bounds: with the check skipped (ch01 §1.3),
    // this reads past the array instead of aborting -- exit code 0
    // just confirms the program didn't abort; the actual value read is
    // unspecified garbage, not asserted here.
    access_unsafely(s, 10);
    print_int(0);
    return 0;
}
