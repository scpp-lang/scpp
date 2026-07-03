int main() {
    int arr[3];
    std::span<int> s = arr;
    int sz = s.size;
    return sz;
}
