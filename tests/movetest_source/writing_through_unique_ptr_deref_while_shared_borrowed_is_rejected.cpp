struct Point { int x; int y; };
int f() {
    std::unique_ptr<Point> p = std::make_unique<Point>();
    const Point& r = *p;
    p->x = 5;
    return r.x;
}
