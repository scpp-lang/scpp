// ch02 §2.1: "struct (already guaranteed Clang-ABI-compatible layout), by
// value or by pointer" are both accepted extern "C" signature types.
struct Point {
    int x;
    int y;
};

extern "C" int sum_point_by_value(Point p) {
    return p.x + p.y;
}

extern "C" int sum_point_by_pointer(Point* p) {
    return p->x + p->y;
}

int main() {
    Point a;
    a.x = 3;
    a.y = 4;
    int by_value = sum_point_by_value(a);
    int by_pointer = sum_point_by_pointer(&a);
    return by_value + by_pointer;
}
