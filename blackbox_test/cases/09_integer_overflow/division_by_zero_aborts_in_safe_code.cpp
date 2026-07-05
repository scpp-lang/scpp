// ch05 §5.8: "Division/modulo by zero ... don't have a wrapped result to
// fall back on ... abort() unconditionally, in every context, safe or
// unsafe alike."
safe int divide(int a, int b) {
    return a / b;
}

int main() {
    return divide(10, 0);
}
