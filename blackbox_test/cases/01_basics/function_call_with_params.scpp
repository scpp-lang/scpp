// Ordinary function calls, including passing the result of one call as an
// argument to another and calling a function defined later in the same
// file (the compiler resolves all signatures before lowering bodies).
int add(int a, int b) {
    return a + b;
}

int mul(int a, int b) {
    return a * b;
}

// add(mul(2, 3), mul(4, 5)) == add(6, 20) == 26
int main() {
    return add(mul(2, 3), mul(4, 5));
}
