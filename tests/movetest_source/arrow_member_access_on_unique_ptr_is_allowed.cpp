import std;
struct Point { int x; int y; };
int f() {
    std::unique_ptr<Point> p = std::make_unique<Point>();
    p->x = 5;
    return p->x;
}
