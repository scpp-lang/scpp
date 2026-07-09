# 10. Reference Implementations (required reading)

- **Circle** (Sean Baxter): a C++ superset with a borrow checker — almost the
  same path as this design.
- **Hylo / Val**: mutable value semantics, an alternative that sidesteps
  borrow-checking complexity.
- **cppfront / cpp2** (Herb Sutter): C++ syntax renewal + safe defaults.
- **Rust `rustc` borrowck + the Polonius papers / NLL RFC**: the mature
  implementation of borrow checking.
- **MLIR**: if the backend wants a high-level dialect to carry ownership/lifetime
  before gradually lowering.

---

*Status: draft v0.1 · pending review before entering M1.*

---

[← Previous: MVP Milestones](ch09-milestones.md) · [Table of Contents](README.md) · [Next: Modules & Libraries →](ch11-modules-and-libraries.md)
