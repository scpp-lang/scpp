struct Point {
    int x;
    int y;
};

int main() {
    Point p;
    p.x = 10;
    int& rx = p.x;
    rx = rx + 1;
    print_int(p.x);
    p.x = 100;
    print_int(p.x);
    return 0;
}
