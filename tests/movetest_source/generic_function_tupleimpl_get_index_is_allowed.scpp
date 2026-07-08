// ch05 §5.14: indexed access into a recursively-defined variadic type,
// reusing template-argument deduction from a base class -- `get<I>(t)`
// explicitly supplies the non-type index `I` (required to disambiguate
// which level of the inheritance chain to match, since an unqualified
// call would only ever match the outermost/direct level), and `Head`/
// `Tail` are deduced from that matched level's own concrete arguments.
template<int Idx, typename... Ts> class TupleImpl;

template<int Idx> class TupleImpl<Idx> {};

template<int Idx, typename Head, typename... Tail>
class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {
public:
    Head value;
};

template<int I, typename Head, typename... Tail>
Head& get(TupleImpl<I, Head, Tail...>& t) { return t.value; }

int main() {
    TupleImpl<0, int, bool, char> t;
    int a = get<0>(t);
    return 0;
}
