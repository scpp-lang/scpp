int f(int& x) {
    return x + 100;
}
int f(int&& x) {
    return x + 200;
}
int main() {
    int y = 5;
    print_int(f(y));
    print_int(f(10));
    return 0;
}
