// ch05 §5.2: "v0.1 treats [`.field` and subscript borrows] whole-root
// conservatively: borrowing a.b is recorded against the root a, and so is
// borrowing a.c -- the two are considered conflicting even though the
// fields never actually overlap in memory." `p.x` and `p.y` are
// disjoint fields, but both borrows are recorded against `p` and must
// conflict.
struct Point {
    int x;
    int y;
};

safe int f() {
    Point p;
    p.x = 1;
    p.y = 2;
    int& rx = p.x;
    int& ry = p.y;
    return rx + ry;
}

int main() {
    return f();
}
