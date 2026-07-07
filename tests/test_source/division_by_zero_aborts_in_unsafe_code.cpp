int divide_unsafely(int a, int b) {
    [[scpp::unsafe]] {
        return a / b;
    }
}
int main() {
    return divide_unsafely(10, 0);
}
