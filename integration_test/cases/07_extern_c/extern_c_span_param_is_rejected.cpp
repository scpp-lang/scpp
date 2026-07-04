// ch02 §2.1: "Rejected: ... std::span, ... none of these have a defined C
// representation."
extern "C" void f(std::span<int> x);

int main() {
    return 0;
}
