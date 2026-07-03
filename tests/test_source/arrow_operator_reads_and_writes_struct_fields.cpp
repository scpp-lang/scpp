struct Point {
    int x;
    int y;
};

int main() {
    std::unique_ptr<Point> p = std::make_unique<Point>();
    p->x = 3;
    p->y = 4;
    print_int(p->x + p->y);
    return 0;
}
