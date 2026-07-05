// ch05 §5.8: "INT_MIN / -1 (the one case where signed division itself
// overflows) ... abort() unconditionally, in every context." Tested in a
// native function, where +/-/* would merely wrap -- division does not.
int divide(int a, int b) {
    return a / b;
}

int main() {
    return divide(-2147483648, -1);
}
