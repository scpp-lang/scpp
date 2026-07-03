int swap_ints(int& a, int& b) {
    int tmp = a;
    a = b;
    b = tmp;
    return 0;
}
int f() {
    int x = 1;
    swap_ints(x, x);
    return 0;
}
