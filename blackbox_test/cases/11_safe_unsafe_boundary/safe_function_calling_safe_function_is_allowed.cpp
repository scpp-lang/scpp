// ch02 boundary table: "safe calls safe | Freely allowed; participates in
// checking normally."
safe int double_it(int x) {
    return x * 2;
}

safe int quadruple(int x) {
    return double_it(double_it(x));
}

int main() {
    return quadruple(5);
}
