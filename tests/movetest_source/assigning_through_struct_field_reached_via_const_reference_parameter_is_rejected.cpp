struct Point {
    int x;
    int y;
};

int mutate_through_const(const Point& p) {
    p.x = 999;
    return 0;
}

int f() {
    return 0;
}
