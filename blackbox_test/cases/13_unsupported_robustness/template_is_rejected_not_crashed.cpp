// ch06 backlog: "Templates / generics, concept" -- not implemented.
template <typename T>
T identity(T x) {
    return x;
}

int main() {
    return identity(5);
}
