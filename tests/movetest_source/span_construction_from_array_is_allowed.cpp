int f() {
    int arr[3];
    arr[0] = 1;
    std::span<int> s = arr;
    return s[0];
}
