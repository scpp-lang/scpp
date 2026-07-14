# Open design issues

This file records language and design questions that have come up in real work
but are deliberately deferred for later consideration.

It is **not** the place for "feature not implemented yet" tracking. That is what
`docs/missings.md` is for. This file is for cases where the implementation
question is blocked on a still-unsettled design decision.

## Thread-safety structural derivation ignores inherited base subobjects

The current thread-safety structural-derivation rules in
[docs/spec/en/04-thread-safety-properties.md](spec/en/04-thread-safety-properties.md)
determine a class's `thread-movable` and `thread-shareable` properties only
from that class's own declared non-static data members. They do not yet account
for inherited ordinary base-class subobjects.

As a result, a derived ordinary class may currently be judged
`thread-shareable` or `thread-movable` even when one of its inherited ordinary
base-class subobjects would prevent that result. This is a pre-existing gap in
the general thread-safety rules, not an interface-specific issue; interface
bases under [docs/spec/en/11-inheritance-and-interfaces.md](spec/en/11-inheritance-and-interfaces.md)
are unaffected because they contribute no non-static data members.

Future work should extend the structural-derivation rules to include inherited
ordinary base-class subobjects when determining a derived class's own
thread-safety properties.

The previously-open question about unchecked integer-to-enum casts was resolved
by the specification in [docs/spec/en/09-enumeration-conversions.md](spec/en/09-enumeration-conversions.md).
