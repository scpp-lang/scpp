int f() {
    int arr[3];
    arr[0] = 1;
    std::span<int> s1 = arr;
    int x = s1[0];
    std::span<int> s2 = arr;
    return x + s2[0];
}
