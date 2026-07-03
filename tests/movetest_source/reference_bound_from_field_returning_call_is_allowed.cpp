struct Point {
    int x;
    int y;
};

int& get_x(Point& p) {
    return p.x;
}

int f() {
    Point p;
    p.x = 7;
    int& r = get_x(p);
    r = 42;
    return p.x;
}
