int f() {
    int arr[3];
    arr[0] = 1;
    std::span<const int> s1 = arr;
    std::span<const int> s2 = arr;
    return s1[0] + s2[0];
}
