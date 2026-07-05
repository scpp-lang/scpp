// Ordinary (non-mutual) recursion: nothing in the spec restricts a
// function from calling itself.
int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

// factorial(5) == 120
int main() {
    return factorial(5);
}
