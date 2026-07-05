safe int f(int x) {
    return x;
}
safe int f(bool b) {
    if (b) {
        return 100;
    }
    return 200;
}
int main() {
    print_int(f(5));
    print_int(f(true));
    print_int(f(false));
    return 0;
}
