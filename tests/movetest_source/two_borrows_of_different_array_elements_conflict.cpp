int f() {
    int values[3];
    int& r0 = values[0];
    int& r1 = values[1];
    r0 = 1;
    r1 = 2;
    return r0 + r1;
}
