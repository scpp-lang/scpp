import std;
int main() {
    int i = 0;
    int sum = 0;
    while (i < 10) {
        std::unique_ptr<int> a = std::make_unique<int>(i);
        sum = sum + i;
        i = i + 1;
    }
    return sum;
}
