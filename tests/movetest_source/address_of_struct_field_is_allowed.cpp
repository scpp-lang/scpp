struct Point { int x; int y; };
int f() {
    Point p;
    int* px = &p.x;
    return 0;
}
