int f() {
    int arr1[3];
    int arr2[3];
    std::span<int> s = arr1;
    s = arr2;
    return 0;
}
