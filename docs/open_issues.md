# Open design issues

This file records language and design questions that have come up in real work
but are deliberately deferred for later consideration.

It is **not** the place for "feature not implemented yet" tracking. That is what
`docs/missings.md` is for. This file is for cases where the implementation
question is blocked on a still-unsettled design decision.

## 2026-07-11: Should scpp allow unchecked C-style casts from an integer to an enum type?

### Context

While implementing `scpp::rand::uniform_int_distribution`'s `make(...)` factory
in PR #119, an early draft used:

```cpp
error code = (error)0;
```

That specific site was corrected to use the named enumerator
`error::empty_range` directly, which is clearer and avoids baking in knowledge
of the enumerator's underlying integer value.

The broader language question remains open, though: should scpp permit raw
numeric casts into an enum type at all when the integer value may not
correspond to any named enumerator?

### Open question

There are at least two plausible directions:

1. **Permit raw C-style casts freely**, matching C++:

   ```cpp
   auto e = (SomeEnum)int_value;
   ```

   This is maximally compatible with C++ expectations, but it is also a classic
   footgun. The resulting enum value may not correspond to any declared
   enumerator at all. That may be "only" a logic bug, but it can also interact
   badly with downstream code such as `switch` statements that assume only named
   cases exist.

2. **Require an explicit named conversion facility instead**, for example:

   ```cpp
   auto e = scpp::enum_cast<SomeEnum>(int_value);
   ```

   In that model, the language and/or stdlib would make the conversion policy
   explicit. The conversion could validate that the integer matches a real
   enumerator and then report failure in some deliberate way rather than
   silently materializing a possibly-invalid enum value.

   The exact failure model is also unresolved: `std::expected`, optional-like
   failure, trap/abort, compile-time rejection in constant contexts, or
   something else.

### Current status

No decision has been made yet on whether scpp should:

- allow unchecked integer-to-enum C-style casts,
- restrict them,
- ban them in favor of a safer explicit facility,
- or split the policy between safe/default and unsafe/escape-hatch forms.

Revisit this when there is time to think through enum invariants, conversion
ergonomics, diagnostics, and compatibility goals more deliberately.
