// ch05 §5.14: single inheritance end-to-end -- field layout is
// flattened (base fields first, see Codegen::declare_class), an
// inherited method not overridden resolves via a synthesized
// forwarding stub, and a multi-level chain (Animal -> Dog -> Puppy)
// resolves a method override statically at each level (Dog's own
// `sound` shadows Animal's; Puppy inherits Dog's override, not
// Animal's original).
class Animal {
public:
    Animal(int legs) { this.legs = legs; return; }
    int leg_count() const { return this.legs; }
    int sound() const { return 0; }
private:
    int legs;
};

class Dog : public Animal {
public:
    Dog(int legs) { this.legs = legs; return; }
    int sound() const { return 1; }
};

class Puppy : public Dog {
public:
    Puppy(int legs) { this.legs = legs; return; }
};

int main() {
    Dog d(4);
    print_int(d.leg_count());
    print_int(d.sound());

    Puppy p(4);
    print_int(p.leg_count());
    print_int(p.sound());
    return 0;
}
