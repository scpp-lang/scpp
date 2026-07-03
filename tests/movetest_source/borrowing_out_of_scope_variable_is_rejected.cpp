int f(bool cond) {
    if (cond) {
        int a = 1;
    }
    const int& r = a;
    return 0;
}
