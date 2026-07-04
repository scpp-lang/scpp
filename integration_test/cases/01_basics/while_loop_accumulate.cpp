// `while` loop with mutation of locals (ch06 supported statement list).
// Sums 1..10 == 55.
int main() {
    int i = 1;
    int sum = 0;
    while (i <= 10) {
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
