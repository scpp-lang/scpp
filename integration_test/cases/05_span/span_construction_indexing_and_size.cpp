// ch03: std::span<T> is a "fat pointer" {data, length} constructed from a
// fixed-size array; `.size` reads the length (a computed field, not a
// method call); `s[i]` reads/writes through the view.
int main() {
    int arr[5];
    arr[0] = 10;
    arr[4] = 20;
    std::span<int> s = arr;
    return s[0] + s[4] + s.size;
}
