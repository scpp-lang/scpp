int sum(std::span<int> s) {
    int total = 0;
    int i = 0;
    while (i < s.size) {
        total = total + s[i];
        i = i + 1;
    }
    return total;
}

int main() {
    int arr[4];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    arr[3] = 4;
    std::span<int> s = arr;
    print_int(sum(s));
    return 0;
}
