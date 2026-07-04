// ch05 §5.8: division by zero aborts even in a native (entirely unsafe)
// function -- unlike +/-/*, this is never merely "skipped".
int divide(int a, int b) {
    return a / b;
}

int main() {
    return divide(10, 0);
}
