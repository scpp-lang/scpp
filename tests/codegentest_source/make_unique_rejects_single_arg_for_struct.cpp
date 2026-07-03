struct Point { int x; int y; };
int main() { std::unique_ptr<Point> a = std::make_unique<Point>(1); return 0; }
