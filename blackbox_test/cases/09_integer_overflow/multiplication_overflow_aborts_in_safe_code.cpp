// ch05 §5.8: multiplication is checked too. 100000 * 100000 == 10^10,
// which overflows a 32-bit int (max ~2.147*10^9).
safe int f(int a, int b) {
    return a * b;
}

int main() {
    return f(100000, 100000);
}
