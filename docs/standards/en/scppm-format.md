# The `.scppm` Module Interface Format

This document specifies `.scppm`, the binary form of a single scpp
module's interface: its declarations, plus, for any generic function, a
serialized form of its body (§2.1). It carries no compiled machine code --
a module's compiled machine code is a separate `.scppa` file (a native
archive of `.scppo` objects), one per target triple, referenced from an
enclosing `.scppkg` package (see
[The `.scppkg` Package Format](scppkg-format.md)).

`.scppm` is referenced from
[ch11 §11.12](../../book/en/ch11-modules-and-libraries.md#1112-the-scppm-scppa-and-scppkg-formats)
of the language specification.

Unlike `.scppkg` (a package bundling an arbitrary number of named files), a
`.scppm` file always holds exactly the same two possible pieces of
content -- so it is not a tar archive. Its bytes are a fixed header,
immediately followed by the interface source and, if present, the
generics block.

## 1. File layout

```
[Header]             -- fixed size, 8 bytes
[interface_length]   -- uint32, little-endian, 4 bytes
[Interface source]   -- interface_length bytes: raw UTF-8 source text
[generics_length]    -- uint32, little-endian, 4 bytes; present only if flags bit 0 is set
[Generics block]     -- generics_length bytes; present only if flags bit 0 is set
```

### Header

A fixed 8-byte header:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 5 | `magic` | ASCII bytes `SCPPM`. |
| 5 | 1 | `major_version` | uint8. This document defines major version `1`. |
| 6 | 1 | `patch_version` | uint8. This document defines patch version `0`. |
| 7 | 1 | `flags` | Bit 0, bits 7-1 (below). |

Bits of `flags`:

| Bit | Meaning |
|---|---|
| 0 | A generics block (§2.1) follows the interface source. `0`: the module exports no generic function, and the `generics_length`/generics-block fields are absent. |
| 7-1 | Reserved, `0` in this version. |

A reader checks `magic` and `major_version` before parsing anything else.
An unrecognized `magic`, or a `major_version` the reader does not support,
is a hard parse failure reported at this point -- never a crash or a
best-effort guess further into the file. A `patch_version` newer than the
reader knows about, and any `flags` bit the reader does not recognize the
meaning of, are not errors: both only ever signal an optional, ignorable
addition -- a reader ignores whatever it does not recognize and proceeds.

A `.scppm` file's own name, with the `.scppm` extension removed, is the
module's dotted name (`mylib.math.scppm` names module `mylib.math`); this
is not repeated anywhere inside the file.

Neither the interface source nor the generics block is compressed:
`.scppm` is designed to be read directly, including as a local build
intermediate. A `.scppm` distributed as part of a package is compressed as
a side effect of `.scppkg`'s own envelope compression (see
[The `.scppkg` Package Format §1-§2](scppkg-format.md)), not by `.scppm`
itself.

## 2. Interface and generics content

Immediately following the header:

| Field | Size | Value |
|---|---|---|
| `interface_length` | 4 bytes, uint32 LE | Byte length of the interface source that follows. |
| Interface source | `interface_length` bytes | Raw UTF-8 text: the module's interface source file. |
| `generics_length` | 4 bytes, uint32 LE | Present only if `flags` bit 0 (§1) is set. Byte length of the generics block that follows. |
| Generics block | `generics_length` bytes | Present only if `flags` bit 0 (§1) is set. A scpp-internal serialized representation of every generic function's body (§2.1). |

A `.scppm` file corresponds to a real C++ module's own BMI (Clang's
`.pcm`, GCC's `.gcm`, MSVC's `.ifc`) and nothing more: interface
declarations plus, for generic functions, their monomorphizable bodies
(§2.1) -- no compiled machine code, and no dependency or system-link
metadata (a real C++ module interface has neither, and neither does this
format). A module's compiled machine code is tracked in a `.scppa` file
(a native archive of `.scppo` objects, one per target triple); which
other modules or system libraries it needs is package-management
metadata, tracked only in the enclosing `.scppkg` package's manifest (see
[The `.scppkg` Package Format §3](scppkg-format.md)). Neither ever lives
in `.scppm`.

A module's interface source may give a full body to some functions and
none to others (bodyless, `extern`, see ch11 §11.7); a function with a
full body compiles directly from the interface source on any target,
while a bodyless one is only linkable where a `.scppo` object, bundled
inside the module's `.scppa` file for that target triple, supplies the
symbol.

### 2.1 Generic (concept-constrained) functions

A generic function ([§5.11](../../book/en/ch05-static-checks.md)) is
monomorphized separately for each concrete type it is called with, at the
call site's own build -- unlike an ordinary function, no single
per-target compiled artifact can serve every caller, since callers may use
concrete types the module's own author never saw. The interface source
therefore still declares every generic function (bodyless, as in the
example below), but its body -- needed by a caller to monomorphize --
lives separately in the generics block, not as source text in the
interface:

```cpp
double total_area(const Shape auto& a, const Shape auto& b);  // declared, no body, in the interface
```

The generics block holds a scpp-internal serialized representation (not
`.scpp` source text) of every generic function's body, sufficient for a
consumer to monomorphize and generate code for whatever concrete types and
target triple it needs. This keeps monomorphization zero-cost (no vtable,
no runtime dispatch -- consistent with
[§5.11](../../book/en/ch05-static-checks.md)) without requiring the
function's logic to be distributed as readable source. The internal
encoding of the generics block is not specified by this document.

## 3. Extensibility

New optional fields are introducible as a `patch_version` bump (§1); a
reader ignores whatever it does not recognize.

`.scppm` carries no signature of its own: it is a language-level format
(a module's compiled interface), not a distribution format. Integrity and
provenance verification for anything shipped to a consumer is entirely
`.scppkg`'s concern, applied once over a whole package -- see
[The `.scppkg` Package Format §4](scppkg-format.md).
