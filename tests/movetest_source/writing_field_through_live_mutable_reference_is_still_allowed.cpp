struct Point { int x; int y; };
int mutate(Point& p) {
    p.x = 999;
    return 0;
}
int f() {
    Point p;
    p.x = 1;
    mutate(p);
    return p.x;
}
