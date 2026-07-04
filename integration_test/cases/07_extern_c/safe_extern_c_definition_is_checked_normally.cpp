// ch02 §2.1: "safe and extern "C" are orthogonal here -- safe extern "C"
// int add(...) { ... } is allowed, and the body is checked exactly like
// any other safe function."
safe extern "C" int add(int a, int b) {
    return a + b;
}

int main() {
    return add(3, 4);
}
