safe int access(std::span<int> s, int i) {
    return s[i];
}
int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    std::span<int> s = arr;
    return access(s, 10);
}
