struct Point {
    int x;
    int y;
};

int sum(Point p) {
    return p.x + p.y;
}

int main() {
    Point p;
    p.x = 10;
    p.y = 20;
    return sum(p);
}
