// `if` / `else if` / `else` (ch06 supported statement list).
int classify(int x) {
    if (x < 0) {
        return -1;
    } else if (x == 0) {
        return 0;
    } else {
        return 1;
    }
}

// classify(5) + classify(0) + classify(-3) + classify(-100) + 10
//   ==   1     +    0       +    (-1)      +     (-1)       + 10  == 9
int main() {
    return classify(5) + classify(0) + classify(-3) + classify(-100) + 10;
}
