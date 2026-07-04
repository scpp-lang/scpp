// ch05 §5.3: "Every reference-typed input parameter ... belongs ...  to
// one shared implicit group" -- with a single reference parameter, an
// elided return-reference's lifetime group is inferred with no
// annotation needed.
safe int& identity(int& x) {
    return x;
}

int main() {
    int y = 42;
    int& r = identity(y);
    return r;
}
