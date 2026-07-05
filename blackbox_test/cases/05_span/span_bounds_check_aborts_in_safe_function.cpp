// ch03/ch08 Q1: "Subscript s[i] carries a runtime bounds check in safe
// code, calling abort() on failure."
safe int oob(std::span<int> s) {
    return s[10];
}

int main() {
    int arr[3];
    std::span<int> s = arr;
    return oob(s);
}
