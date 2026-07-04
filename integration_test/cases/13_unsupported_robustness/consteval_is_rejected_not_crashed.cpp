// ch06 backlog: "Implementation of consteval functions ... design only so
// far."
consteval int square(int x) {
    return x * x;
}

int main() {
    return square(5);
}
