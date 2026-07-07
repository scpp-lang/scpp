# 7 Dereference and Member Access

## 7.1 Class dereference operators [over.deref]

(1) A class may declare a non-static member function named `operator*`.

(2) A declaration of `operator*` is ill-formed unless:

  (2.1) it has no parameters; and

  (2.2) its declared return type is either "lvalue reference to `T`" or
  "lvalue reference to `const T`", for some object type `T`.

(3) An expression of the form `*E`, where `E` is of class type `C` or of
reference to class type `C`, is equivalent to a member-function call whose
callee is `C::operator*` selected for `E` by the ordinary rules for member
access and `const` qualification, and whose receiver is `E`.

[Note: this clause introduces no distinct ownership, aliasing, or lifetime
rule of its own. If `C::operator*` returns a reference, that reference is
governed by the same rules that already govern any other reference returned
from a member function call. — end note]

(4) An expression of the form `*E`, where `E` is of pointer type, remains
governed by [expr.unary.op] and by this document's existing requirements on
pointer dereference, including [§5.1](01-unsafe.md#51-attributes-dclattrscppunsafe)
(5.1) and (6).

## 7.2 Arrow expressions [expr.ref.scpp.arrow]

(1) An expression of the form `E1->E2` is equivalent to `(*E1).E2`.

(2) This document provides no distinct `operator->` semantics beyond (1).

[Note: unlike the C++ standard's separate `operator->` protocol, this
document always performs exactly the one rewrite in (1). If `E1` has class
type and `*E1` is well-formed by [§7.1](03-dereference-and-member-access.md#71-class-dereference-operators-overderef),
that same result is what the member access in (1) operates on. — end note]

```cpp
class Box {
    int value;
public:
    int& operator*() { return value; }
    const int& operator*() const { return value; }
};

int f(Box& b) {
    *b = 1;          // (3): equivalent to b.operator*() = 1;
    return *b;       // (3): equivalent to b.operator*();
}
```

---

[← Previous: Ownership, Initialization, and Move](02-ownership-and-move.md) · [Table of Contents](README.md) · [Next: Thread-Safety Properties →](04-thread-safety-properties.md)
