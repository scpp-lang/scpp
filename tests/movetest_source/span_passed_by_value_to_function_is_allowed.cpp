int sum_first_two(std::span<int> s) {
    return s[0] + s[1];
}
int f() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    std::span<int> s = arr;
    return sum_first_two(s);
}
