template<typename Sig>
class Holder;

template<typename R, typename... Args>
class Holder<R(Args...) const &&> {
public:
    int arity() const { return 1; }
};

int main() {
    Holder<int(int) const &&> h;
    return h.arity() - 1;
}
