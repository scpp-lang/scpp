# 10 Modules and Namespaces

## 10.1 General [module.unit], [namespace.def]

(1) Except as modified by this clause, [module.unit] through
[module.private.frag] and [namespace.def] through [namespace.alias]
apply unchanged to a SCPP26 program. SCPP26 reuses C++26's module and
namespace syntax verbatim -- `export module`, `module`, `import`,
`export`, and `namespace`, including the nested *namespace-definition*
form `namespace A::B::C { ... }` -- introducing no new keyword,
operator, or other token.

(2) This clause requires every exported declaration to appear in a
namespace determined by its own module's name
([§10.3](#103-export-declarations-and-the-required-namespace-moduleinterface)),
gives an *import-declaration* precise visibility rules, including for a
module's own partitions
([§10.4](#104-import-declarations-re-export-and-cross-module-name-merging-moduleimport)),
withholds two namespace forms C++26 otherwise permits
([§10.5](#105-prohibited-namespace-forms-namespaceunnamed-namespaceudir)),
and restricts name lookup for a function call whose *postfix-expression*
is an unqualified name
([§10.6](#106-name-lookup-for-unqualified-function-calls-basiclookupunqual-basiclookupargdep)).

## 10.2 Module declaration forms and partitions [module.unit]

(1) A translation unit whose first declaration is `export module`
*module-name*`;` is that module's **primary interface unit**. A module
has exactly one primary interface unit.

(2) A *module-name* may be followed by `:` *partition-name*, naming the
translation unit as one **partition** of that module, distinct from its
primary interface unit and from every other partition of the same
module:

  (2.1) `export module` *module-name*`:`*partition-name*`;` declares an
  **interface partition**, which may itself export declarations
  ([§10.3](#103-export-declarations-and-the-required-namespace-moduleinterface));

  (2.2) `module` *module-name*`:`*partition-name*`;` (no leading
  `export`) declares an **implementation partition**, which shall not
  export any declaration; an *export-import-declaration*
  ([§10.4](#104-import-declarations-re-export-and-cross-module-name-merging-moduleimport))
  naming such a partition is ill-formed.

(3) A partition is part of its module but is not itself importable by
name outside that module: only a translation unit that begins with
`export module` *module-name*`;`, or with `module`/`export module`
*module-name*`:`*other-partition-name*`;` for the same *module-name*,
may contain an *import-declaration* naming one of that module's
partitions.

[Note: informally, only a file that is itself part of module M -- its
primary interface unit, or any one of its partitions -- may `import`
one of M's other partitions; a file outside M can only `import` M as a
whole
([§10.4](#104-import-declarations-re-export-and-cross-module-name-merging-moduleimport)).
— end note]

```cpp
// geometry.scpp -- primary interface unit of module "geometry"
export module geometry;

export import :distance;   // re-exports the partition below, see §10.4

namespace geometry {
    export int scale(int x) { return x * 10; }
}
```

```cpp
// geometry_distance.scpp -- an interface partition of the same module
export module geometry:distance;

namespace geometry {
    export int manhattan(int ax, int ay, int bx, int by) {
        int dx = ax - bx; if (dx < 0) dx = -dx;
        int dy = ay - by; if (dy < 0) dy = -dy;
        return dx + dy;
    }
}
```

```cpp
// main.scpp -- a translation unit outside module "geometry"
import geometry;

int main() {
    return geometry::manhattan(0, 0, 3, 4) + geometry::scale(2);  // 27
}
```

## 10.3 Export declarations and the required namespace [module.interface]

(1) `export`, applied to a declaration ([module.interface]), marks it
visible to a translation unit that imports the module -- or, for a
declaration of a partition, imports that partition
([§10.4](#104-import-declarations-re-export-and-cross-module-name-merging-moduleimport)).
A declaration to which `export` does not apply is **private** to its
own module: reachable from elsewhere within the same module (by a
qualified or unqualified name, as ordinary lookup permits) but never
through an *import-declaration*.

(2) If `export` appertains to a declaration in a translation unit whose
first declaration is not one of the forms in 10.2(1) or 10.2(2.1), the
program is ill-formed.

[Note: only a primary interface unit or an interface partition may
export anything. An implementation partition may not (10.2(2.2)), and a
translation unit with no module declaration at all has no module to
export from. — end note]

(3) Let ns(*module-name*) be the namespace obtained by splitting a
*module-name* at each `.` and treating each resulting identifier as one
further level of nesting (so ns(`org.lotx.cmath`) denotes the namespace
`org::lotx::cmath`); for a partition, ns(*module-name*) is derived the
same way from the portion of its *module-name* preceding the `:`. If
`export` appertains to a declaration, that declaration shall be a
member of ns(*module-name*) or of a namespace nested, directly or
indirectly, inside ns(*module-name*); otherwise the program is
ill-formed.

[Note: (2) and (3) are independent, both-mandatory conditions: a
declaration that satisfies (3) but appears in a translation unit that
fails (2) is still ill-formed, and vice versa. A namespace that is a
proper prefix of ns(*module-name*), or that diverges from it at any
level, does not satisfy (3), even though it may share a leading
sequence of identifiers with ns(*module-name*). — end note]

(4) A declaration to which `export` does not apply is unconstrained by
(3) and may be a member of any namespace, or of none.

```cpp
export module org.lotx.cmath;

namespace org::lotx::cmath {
    export int abs_int(int x) { return x < 0 ? -x : x; }   // OK: (3)

    namespace detail {
        export int clamp(int x, int lo, int hi) {           // OK: (3),
            return x < lo ? lo : (x > hi ? hi : x);          // nested deeper
        }
    }
}

namespace org::lotx {
    int helper() { return 0; }               // OK: (4), not exported

    export int wrong() { return 1; }         // ill-formed: (3), 'org::lotx'
                                               // is a proper prefix, not
                                               // ns(module-name) or deeper
}

export int no_namespace_at_all() { return 2; }  // ill-formed: (3)
```

## 10.4 Import declarations, re-export, and cross-module name merging [module.import]

(1) A plain *import-declaration* -- `import` *module-name*`;`, or,
within module M, `import :`*partition-name*`;` for one of M's own
partitions -- makes the imported unit's exported declarations visible
in the importing translation unit only; it is not transitive. Importing
something that has itself imported (plainly) a third module or
partition grants no visibility into that third module or partition.

(2) An *export-import-declaration* -- `export import` *module-name*`;`,
or, within an interface partition or the primary interface unit of
module M, `export import :`*partition-name*`;` -- has the same
visibility effect as (1) and, in addition, makes every declaration
thereby made visible also visible, transitively, to any translation
unit that imports the unit containing the *export-import-declaration*.

(3) `import :`*partition-name*`;`, within module M, makes visible -- to
the importing translation unit only, not transitively -- every
declaration of that partition, whether or not `export` appertains to
it. `export import :`*partition-name*`;` is ill-formed if that
partition is an implementation partition (10.2(2.2)).

[Note: a plain `import` of a partition therefore differs from a plain
`import` of another module by name: the former exposes all of a
partition's declarations within the same module, private ones
included, while the latter exposes only what that module itself
exports (1). — end note]

(4) A declaration made visible to a translation unit by one or more
*import-declaration*s is merged into that translation unit's namespaces
as if it had been declared, at the same namespace-scope position, in
the same translation unit. If two such declarations, or one such
declaration and a declaration of the importing translation unit
itself, would be ill-formed as a redeclaration or redefinition had both
instead appeared literally in one translation unit ([basic.def.odr],
[dcl.fct]), the program is ill-formed; otherwise they take part in
overload resolution ([over.match]) together, exactly as
same-signature and differing-signature declarations of the same name
do within a single translation unit.

[Note: there is no dedicated diagnostic for a name made ambiguous by
import; a colliding pair of imported declarations is ill-formed for
the same reason, and is diagnosed the same way, as a matching pair
written directly in one file would be. Two modules may legitimately
export declarations of the same qualified name with different
signatures -- merging into one overload set, by (4) -- when their two
required namespaces ((3) of
[§10.3](#103-export-declarations-and-the-required-namespace-moduleinterface))
are related by nesting, for instance because one module's name is a
prefix of the other's. — end note]

```cpp
// org.scpp -- primary interface unit of module "org"
export module org;

namespace org::lotx {
    export int describe(int x) { return x; }
}
```

```cpp
// org_lotx.scpp -- primary interface unit of module "org.lotx"
export module org.lotx;

namespace org::lotx {
    // OK: (4), a legitimate overload, not a collision
    export int describe(int x, int y) { return x + y; }
}
```

```cpp
// main.scpp
import org;
import org.lotx;

int main() {
    return org::lotx::describe(5) + org::lotx::describe(10, 27);  // 42
}
```

## 10.5 Prohibited namespace forms [namespace.unnamed], [namespace.udir]

(1) An *unnamed-namespace-definition* ([namespace.unnamed]) is
ill-formed.

(2) A *using-directive* ([namespace.udir]) is ill-formed.

[Note: a *using-declaration* naming a base class member
([namespace.udecl]) in a class's *member-specification* is unaffected
by (2) and continues to be well-formed (see
[§11](11-inheritance-and-interfaces.md)); (2) applies only to a
*using-directive* naming a namespace. — end note]

```cpp
namespace {
    int x = 0;      // ill-formed: (1)
}

namespace foo {
    int y = 0;
}

using namespace foo;   // ill-formed: (2)
```

## 10.6 Name lookup for unqualified function calls [basic.lookup.unqual], [basic.lookup.argdep]

(1) Argument-dependent lookup ([basic.lookup.argdep]) is not performed
for any function call.

(2) Let S be the namespace that most closely encloses the point of a
function call whose *postfix-expression* is an unqualified
*id-expression* naming that function (ignoring, for this purpose, any
intervening block, function, class, or lambda scope). Unqualified
lookup ([basic.lookup.unqual]) for that *id-expression* considers only:

  (2.1) declarations found in a block, function-parameter, or class
  scope enclosing the point of the call, exactly as in unmodified C++;

  (2.2) declarations that are direct members of S; and

  (2.3) declarations that are direct members of the global namespace.

No other namespace is considered, even one that lexically encloses S;
in particular, a namespace enclosing S other than the global namespace
is never searched, though [basic.lookup.unqual] would otherwise also
examine it.

[Note: this restriction applies only to the unqualified lookup
performed for a function call's own callee-name. Unqualified lookup for
a type, variable, or other non-call use of a name is unaffected and
continues to examine every namespace enclosing the point of use,
exactly as in unmodified C++. A *qualified-id*, including one beginning
with `::` ([basic.lookup.qual]), is likewise unaffected: `N::f()` and
`::f()` are looked up exactly as in unmodified C++ regardless of (2).
— end note]

```cpp
int global_helper() { return 100; }

namespace outer {
    int outer_helper() { return 10; }

    namespace inner {
        struct Widget { int v; };

        int use_it() {
            Widget w{};              // OK: unqualified type lookup is
            w.v = 1;                 // unaffected by (2) and climbs to
                                       // 'outer' normally
            return global_helper();  // OK: (2.3)
        }

        int bad() {
            return outer_helper();   // ill-formed: (2); 'outer' is
        }                             // neither (2)'s S ('inner') nor
    }                                  // the global namespace
}
```

```cpp
namespace ns {
    struct Tag { int v; };
    int process(Tag t) { return t.v; }
}

int use(ns::Tag t) {
    return process(t);   // ill-formed: (1); no argument-dependent
}                          // lookup, even though 'process' and 'Tag'
                            // are both members of 'ns'
```

---

[← Previous: Inheritance and Interfaces](11-inheritance-and-interfaces.md) · [Table of Contents](README.md)
