template<typename T>
concept HasGet = requires(T t) { t.get(); };

class Num {
public:
    int v;
    int get() const {
        return this->v;
    }
};

int sum_two(const HasGet auto&... args) {
    return (args.get() + ...);
}

int main() {
    Num a;
    a.v = 3;
    Num b;
    b.v = 4;
    print_int(sum_two(a, b));
    return 0;
}
