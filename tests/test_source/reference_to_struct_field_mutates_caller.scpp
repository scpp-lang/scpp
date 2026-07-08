struct Point {
    int x;
    int y;
};

int inc(int& v) {
    v = v + 1;
    return 0;
}

int main() {
    Point p;
    p.x = 10;
    p.y = 20;
    print_int(p.y);
    int& rx = p.x;
    inc(rx);
    print_int(rx);
    return 0;
}
