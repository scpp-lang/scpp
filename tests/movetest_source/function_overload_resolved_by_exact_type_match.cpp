safe int f(int x) {
    return x;
}
safe int f(bool b) {
    return 1;
}
safe int g() {
    return f(5) + f(true);
}
