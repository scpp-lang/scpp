int f(bool cond) {
    if (cond) {
        int arr[3];
    }
    std::span<int> s = arr;
    return s[0];
}
