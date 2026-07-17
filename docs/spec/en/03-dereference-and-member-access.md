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
from a member function call. In particular, if that reference is derived from
the receiver, any borrow or reborrow through the receiver's implicit object
parameter remains governed by [§6.2](02-ownership-and-move.md#62-ownership-move-state-and-reborrows-basiclife)
(7)-(12) and (23). — end note]

(4) An expression of the form `*E`, where `E` is of pointer type, remains
governed by [expr.unary.op] and by this document's existing requirements on
pointer dereference, including [§5.1](01-unsafe.md#51-the-scppunsafe-attribute)
(5.1) and (6).

## 7.2 Class arrow operators [over.ref]

(1) A class may declare a non-static member function named `operator->`.

(2) A declaration of `operator->` is ill-formed unless:

  (2.1) it has no parameters; and

  (2.2) its declared return type is either a pointer type, a class type,
  or a reference to class type.

(3) A user-written expression of the form `E.operator->()` is an ordinary
member-function call. It yields the declared result of that call and does
not, by itself, perform the special arrow-expression protocol of §7.3.

[Note: consequently, `auto raw = p.operator->();` exposes an ordinary value of
whatever type `operator->` returns. If that value is a raw pointer, all
ordinary pointer rules still apply to later uses of `raw`; the safe
`E1->E2` carve-out in §7.3 does not apply. — end note]

## 7.3 Arrow expressions [expr.ref.scpp.arrow]

(1) An expression of the form `E1->E2`, where `E1` is of pointer type, is
equivalent to `(*E1).E2`.

(2) An expression of the form `E1->E2`, where `E1` is of class type `C` or
reference to class type `C`, is resolved as follows:

  (2.1) If overload resolution selects a member `C::operator->` for `E1`,
  the implementation shall evaluate that call and examine its result.

  (2.2) If that result has pointer type, `E1->E2` is completed by member
  access through that pointer.

  (2.3) If that result has class type or reference to class type `D`, and
  overload resolution selects a member `D::operator->` for that result,
  the implementation shall apply (2.1)-(2.3) again to that new result.

  (2.4) Otherwise, the program is ill-formed.

A program is ill-formed under (2.4) if a selected `operator->` result is
neither a pointer nor a class/reference-to-class value for which the next
`operator->` step is well-formed. An implementation should diagnose this as an
`operator->` chain that did not yield a pointer.

(3) If `E1` is of class type or reference to class type and no `operator->`
is selected for it under (2.1), the program is ill-formed.

[Note: unlike this document's previously shipped implementation behavior, SCPP26
does not provide any blanket fallback from `E1->E2` to `(*E1).E2` for a class
type that merely defines `operator*`. This matches real C++: class-type `->`
requires an explicit `operator->`. Existing library wrappers that intend to
support `->`, including `std::unique_ptr`, therefore need an explicit
`operator->` declaration as a follow-up migration. — end note]

(4) If both `operator*` and `operator->` are available for the same class,
`E1->E2` uses `operator->` and is governed by (2); the presence of
`operator->` does not change the meaning of `*E1`, which remains governed by
§7.1.

(5) For the purposes of this subclause, a selected `operator->` invocation is
**receiver-tied** if its declarator bears `[[scpp::lifetime(name)]]` and that
annotation ties the result to the implicit object parameter under
[§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
(23).

(6) A receiver-tied annotation constrains lifetime only. By itself it does not
prove that the raw pointer value produced by `operator->` is valid, and it does
not relax the ordinary `[[scpp::unsafe]]` requirement on raw-pointer
dereference under [§5.1](01-unsafe.md#51-the-scppunsafe-attribute) or
[§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
(21).

Because [§6.2](02-ownership-and-move.md#cross-function-lifetime-groups-dclattrscpplifetime)
permits `[[scpp::lifetime(name)]]` only on reference, pointer, and span return
positions, an `operator->` that returns a class prvalue may participate in the
chaining protocol of (2), but that step is not receiver-tied.

(7) The safe case for `E1->E2` reuses the same receiver-rooted borrow
discipline that already governs a class `operator*` under §7.1 together with
[§6.2](02-ownership-and-move.md#62-ownership-move-state-and-reborrows-basiclife)
(7)-(12) and (23). For each selected `operator->` step in (2) that is
receiver-tied, the implementation shall treat the access eventually obtained
through that step as derived from that step's implicit object parameter. While
any borrow or reborrow derived from the overall `E1->E2` expression remains
live, the existing binding or root place that supplied each such implicit
object parameter remains subject to those ordinary restrictions: it may not be
moved-from, reinitialized through that binding, or allowed to end its lifetime
in a way that would invalidate the derived access.

(8) The final implicit raw-pointer dereference performed only to complete one
`E1->E2` expression is treated as safe, and therefore does not require an
unsafe context, if and only if both of the following hold:

  (8.1) every selected `operator->` invocation in that same chain is
  receiver-tied; and

  (8.2) for every such selected invocation, the implementation enforces the
  receiver-rooted borrow discipline of (7) on the corresponding receiver object
  or root place.

When (8.1)-(8.2) hold, the implementation may rely on the wrapper type's own
invariant that the pointer returned from each selected `operator->` step
remains valid while the corresponding receiver object continues to satisfy the
state constraints that §6.2 already enforces for derived borrows and
reborrows.

(9) If any selected `operator->` invocation in that chain is not receiver-tied,
the safe case of (8) does not apply to that `E1->E2` expression, and the final
implicit raw-pointer dereference in (2.2) is governed by the ordinary
raw-pointer rules of [§5.1](01-unsafe.md#51-the-scppunsafe-attribute). In that
case the enclosing `E1->E2` expression is well-formed only in an unsafe
context.

(10) Any raw pointer value produced only while following an `operator->` chain
under (2) exists solely as an internal transient operand of that same
`E1->E2` expression. That internal pointer is consumed immediately by the
resulting member access or method call and is not a program value that can be
named, stored, passed as an argument, returned, or otherwise observed as a
separate expression.

[Note: this is the safety-critical invariant behind (8): the safe case does not
grant general permission to obtain or manipulate a raw pointer, nor does it
re-prove the wrapper's pointer validity from scratch. It grants only one
compiler-synthesized dereference whose raw-pointer operand is never exposed as a
user-visible value, while the compiler separately enforces on the receiver
object the same derived-borrow restrictions that already make `operator*`
sound. This is analogous to how checked indexing may perform internal pointer
arithmetic without exposing an unchecked raw pointer. — end note]

(11) A separate user-written expression that obtains a pointer by other means,
including `p.operator->()` or `&(*p)`, is outside (2) and (6)-(8). Such an
expression is governed entirely by the ordinary rules for the expression the
program actually wrote, including any `[[scpp::unsafe]]` requirement.

The following declarations and expressions are well-formed:

```cpp
struct Node {
    int value{};
};

struct OwningPtr {
    Node* ptr{};
public:
    Node* operator->() [[scpp::lifetime(self)]] { return ptr; }
    const Node* operator->() const [[scpp::lifetime(self)]] { return ptr; }
};

int read_value(OwningPtr& p) {
    return p->value;      // OK: safe by (8), with the same receiver-rooted borrow discipline as `operator*`
}

struct Inner {
    Node* ptr{};
public:
    Node* operator->() [[scpp::lifetime(inner)]] { return ptr; }
};

struct Outer {
    Inner inner{};
public:
    Inner& operator->() [[scpp::lifetime(outer)]] { return inner; }
};

int read_chain(Outer& o) {
    return o->value;      // OK: both selected operator-> steps are receiver-tied and both receivers are tracked by (7)
}

struct UncheckedPtr {
    Node* ptr{};
public:
    Node* operator->() { return ptr; }
};

int read_unchecked(UncheckedPtr& p) {
    [[scpp::unsafe]] {
        return p->value;  // OK only here: (9)
    }
}
```

[Note: `OwningPtr` is not safe because `[[scpp::lifetime(self)]]` somehow
proves the stored raw pointer field is globally valid. It is safe only to the
same extent that an analogous `Node& operator*()` wrapper is already safe: the
type's own invariant is that its internal pointer remains valid while the
wrapper object itself remains in the required state, and the compiler's role is
to enforce under (7)-(8) that `p` is not moved-from, reinitialized, or allowed
to die while a derived access from `p->...` remains live. — end note]

The following declarations or expressions are ill-formed:

```cpp
struct BadSig {
    Node* operator->(int) { return nullptr; }
};
// ill-formed: `operator->` shall have no parameters

struct BadReturn {
    int operator->() { return 0; }
};
// ill-formed: `operator->` shall return a pointer, class, or reference-to-class type

struct LegacyBox {
    Node value{};
public:
    Node& operator*() { return value; }
    const Node& operator*() const { return value; }
};

int bad_legacy(LegacyBox& b) {
    return b->value;
}
// ill-formed: a class type does not get `->` through `operator*`; an explicit `operator->` is required

struct Proxy {};

struct BrokenChain {
    Proxy operator->() { return {}; }
};

int bad_chain(BrokenChain& p) {
    return p->value;
}
// ill-formed: the selected `operator->` chain does not yield a pointer

struct HalfCheckedInner {
    Node* ptr{};
public:
    Node* operator->() { return ptr; }
};

struct HalfCheckedOuter {
    HalfCheckedInner inner{};
public:
    HalfCheckedInner& operator->() [[scpp::lifetime(outer)]] { return inner; }
};

int bad_safety(HalfCheckedOuter& p) {
    return p->value;
}
// ill-formed outside an unsafe context: one selected `operator->` step is not receiver-tied
```

---

[← Previous: Ownership, Initialization, and Move](02-ownership-and-move.md) · [Table of Contents](README.md) · [Next: Thread-Safety Properties →](04-thread-safety-properties.md)
