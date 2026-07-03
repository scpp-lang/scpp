struct Point { int x; int y; };
int f() {
    Point p;
    p.x = 1;
    p.y = 2;
    const Point& whole = p;
    p.y = 99;
    return whole.x;
}
