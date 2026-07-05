int f(int x) {
    return x;
}
int f(bool b) {
    return 1;
}
int g() {
    return f(5) + f(true);
}
