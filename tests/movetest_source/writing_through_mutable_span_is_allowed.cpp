int f() {
    int arr[3];
    std::span<int> s = arr;
    s[0] = 99;
    return arr[0];
}
