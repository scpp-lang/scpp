struct Inner {
    int v;
};

struct Outer {
    Inner inner;
    int extra;
};

int main() {
    Outer o;
    o.inner.v = 5;
    o.extra = 2;
    return o.inner.v + o.extra;
}
