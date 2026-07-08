int sum(std::span<const int> s) {
    int total = 0;
    int i = 0;
    while (i < s.size) {
        total = total + s[i];
        i = i + 1;
    }
    return total;
}

int main() {
    int arr[3];
    arr[0] = 5;
    arr[1] = 10;
    arr[2] = 15;
    std::span<const int> s = arr;
    print_int(sum(s));
    print_int(s[1]);
    return 0;
}
