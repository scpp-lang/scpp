# SCPP project-scale build system design

Status: research + reviewed design draft before implementation

## 0. Scope and headline

This document proposes how scpp should grow from today's single-file / single-module CLI into a **project-scale build system** suitable for real multi-package, multi-module codebases.

The core recommendation is:

1. keep **zero-config single-file operation** as the non-project case;
2. use an **optional manifest** (`scpp.toml`) as the boundary for package/workspace project mode;
3. make the public UX **Cargo-like and integrated** (`scpp build`, `scpp run`, `scpp package`, workspaces, profiles, dependency declarations);
4. keep the internal model **artifact-first**, built around scpp's existing `.scppm` / `.scppa` split rather than around CMake-style hand-maintained target graphs.

In short: **Cargo-like user experience, CMake-like project scale, scpp-native artifact semantics.**

## 1. Current scpp starting point

Today's reference state matters because the build-system design should extend it, not fight it.

### 1.1 Existing language/package facts

From the existing module/library chapter and binary-format specs:

- scpp already defines **modules** as the compilation/import unit, using real C++20-style `export module`, `module`, `import`, and partitions.[^modules-book]
- `.scppm` already exists as the compiled **module interface** format: one file per module, carrying declarations and the generics block, but **no machine code and no dependency metadata**.[^scppm-spec]
- `.scppa` already exists as the compiled **native machine-code archive** for one module and one target triple.[^modules-book]
- `.scppkg` already exists as the specified **distribution package** format, bundling one or more modules plus dependency metadata and per-module native link requirements.[^scppkg-spec]
- the current language spec explicitly says **package management / dependency resolution / registries** are tooling concerns, not language grammar concerns.[^modules-out-of-scope]

This is a strong starting point: scpp already has the right artifact boundaries for scalable builds.

### 1.2 Existing CLI facts

The current CLI already has two useful layers:

- `scpp <file.scpp> ...` builds a single file directly to an executable.[^cli-current]
- `scpp build-module <file.scpp> --interface-out X.scppm --archive-out Y.scppa` builds one module's compiled artifacts explicitly.[^cli-current]

That means project build does **not** need to invent a new low-level compiler primitive. It mainly needs an orchestrator above primitives that already make sense.

### 1.3 Existing manifestless directory-build precedent

The paused `dev-agent/project-build-mode` branch is also important context:

- bare `scpp` with no arguments looks for `main.scpp` and/or `lib.scpp` in the current directory;
- `main.scpp` builds an executable named after the sanitized directory name;
- `lib.scpp` builds `<name>.scppm` + `lib<name>.scppa` and requires the module name to match the sanitized directory name.[^project-mode-branch]

This is useful as a **narrow manifestless directory-build precedent**, but it cannot by itself express:

- path dependencies,
- multi-package repositories,
- profiles,
- native-library requirements,
- multiple binaries,
- publishable package identity.

So the central design problem is not whether such convenience exists. It is whether manifestless convention is enough for large projects. I think the answer is **no**.

## 2. Research findings from mature systems

## 2.1 Cargo: the most relevant high-level precedent

Cargo's shape is the strongest precedent for scpp's public UX.

Verified from the official Cargo docs:

- each package has a `Cargo.toml` manifest; the manifest is the unit Cargo discovers and compiles.[^cargo-manifest]
- dependency declarations cleanly separate **path**, **git**, and versioned registry dependencies.[^cargo-deps]
- workspaces provide a shared root, shared lockfile, shared output directory, and workspace-level dependency/profile inheritance.[^cargo-workspaces]
- profiles are first-class (`dev`, `release`, custom profiles), and Cargo chooses defaults by command (`cargo build` => `dev`, `--release` => `release`).[^cargo-profiles]
- `Cargo.lock` records exact resolved dependencies and is maintained by Cargo rather than hand-edited.[^cargo-lock]
- Cargo supports **build scripts** (`build.rs`) and **features**, but both exist because Rust has a mature host-execution model and a language-level conditional-compilation system.[^cargo-build-scripts][^cargo-features]

I also verified a minimal local Cargo workspace:

```console
$ cargo build --workspace
   Compiling lib v0.1.0 (.../lib)
   Compiling app v0.1.0 (.../app)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.43s
```

and Cargo created a single workspace-root `Cargo.lock` and shared `target/` directory.[^local-cargo-check]

### Cargo lessons for scpp

Keep:

- manifest-per-package;
- workspace root with shared output/cache;
- direct dependency declarations;
- explicit profiles;
- package-oriented thinking rather than file-oriented thinking.

Do **not** copy directly:

- `build.rs`-style arbitrary host code in v1;
- Cargo features / conditional dependency activation, because scpp deliberately has no preprocessor and no existing conditional-compilation design.[^modules-book]

## 2.2 CMake: the best precedent for composition and transitive native requirements

Verified from the official CMake docs:

- CMake is explicitly a **cross-platform buildsystem generator**: it generates a native buildsystem, then either CMake or the native build tool executes it.[^cmake-manual]
- `add_subdirectory()` composes per-directory build descriptions and processes the child `CMakeLists.txt` immediately.[^cmake-add-subdirectory]
- `target_link_libraries()` propagates **usage requirements** transitively; linked libraries can affect both compilation and linking of downstream targets.[^cmake-target-link]
- `find_package()` exists precisely to discover externally provided packages and surface imported targets/configuration into the local build graph.[^cmake-find-package]
- CMake Presets provide structured configure/build/test workflows and named configurations without requiring users to remember long command lines.[^cmake-presets]

I also verified the two-step CMake workflow locally:

```console
$ cmake -S . -B build
-- Configuring done
-- Generating done
-- Build files have been written to: .../build

$ cmake --build build
[100%] Built target app
```

This matters because it demonstrates the core generator split very concretely.[^local-cmake-check]

### CMake lessons for scpp

Keep:

- explicit composition for large repos;
- transitive native dependency propagation;
- the idea that "usage requirements" are first-class build metadata.

Do **not** copy directly as the primary user model:

- a separate build-language / generator-first workflow.

Why not? Because CMake's raison d'etre is cross-language, cross-toolchain, cross-IDE generation. scpp's project build system is narrower: it is primarily about **building scpp packages whose semantics already revolve around `.scppm`, `.scppa`, module imports, and package metadata**.

## 2.3 Bazel / bzlmod: strict dependency visibility and lockable external resolution

Verified from Bazel's official external-dependencies docs:

- Bazel's modern external system is based on `MODULE.bazel` and versioned modules.[^bazel-overview]
- Bazel resolves the full transitive graph before fetching/building and emphasizes deterministic version resolution, strict dependency visibility, and lockfiles/vendor mode.[^bazel-overview]

### Bazel lessons for scpp

The most valuable Bazel idea for scpp is **strict direct-dependency visibility**:

- if package `A` depends on package `B`, and `B` depends on `C`, package `A` should not implicitly gain compile-time visibility of `C`'s modules.
- transitive dependencies still matter for final linking, but direct declarations should remain the compile-time contract.

That rule gives scpp more predictable project-scale behavior and matches both Bazel and Cargo instincts.

## 2.4 Go modules: decentralized identity and simple package-root manifests

Verified from the official Go module docs:

- a module is identified by a path declared in `go.mod` at the module root.[^go-mod-ref]
- Go uses module-level dependency management, not per-file dependency metadata.[^go-managing-deps]
- `go.mod` and `go.sum` live at the module root and are intended to be committed.[^go-managing-deps]

### Go lessons for scpp

The useful lesson is not Go's exact file format. It is the **identity model**:

- package identity should live at the package root;
- module names inside the package may be more granular than package identity;
- external package distribution can remain decentralized later without changing the local-build model.

That fits scpp well because scpp already distinguishes package metadata (`.scppkg`) from module names (`mylib.math`, `org.example.net`, etc.).

## 2.5 Zig build system: valuable, but the wrong v1 shape for scpp

Verified from Zig's official docs:

- Zig positions `build.zig` as the extra abstraction layer when command lines become too long, dependencies appear, or concurrency/caching/configuration are needed.[^zig-build]
- Zig's build graph is an explicit DAG of steps, with caching and concurrency built in.[^zig-build]

### Zig lessons for scpp

The attractive part is the DAG/caching mindset.

The unattractive part for scpp v1 is **"build system as user code in the language itself"**:

- scpp is not self-hosting;
- scpp does not yet have a mature stable stdlib/runtime for build scripting;
- evaluating arbitrary project-provided scpp code at configure time would be a large security, determinism, and bootstrapping problem;
- the user asked for something CMake-like / Cargo-like for real builds, not for a second general-purpose execution environment.

So: take Zig's graph/caching ambitions, but **not** Zig's "build configuration is a program" model.

## 2.6 Meson: good declarative ergonomics for native dependencies

Verified from Meson's dependency docs:

- `dependency('zlib')` provides a declarative, user-friendly layer above pkg-config/CMake/system discovery; downstream targets consume dependency objects rather than manual flags.[^meson-deps]
- Meson also supports system-fallback/subproject patterns for cases where the dependency is missing from the host system.[^meson-deps]

### Meson lessons for scpp

Meson's best lesson is for **native / foreign dependency declarations**:

- scpp should prefer a declarative manifest form over raw linker flags whenever possible;
- but v1 can start with a simpler manual declaration model and leave richer discovery as a later **scpp-native** layer rather than committing to third-party discovery tools as the long-term answer.

## 3. Design principles for scpp

From the research and scpp's current architecture, I think the build system should follow these rules.

1. **Package-oriented, not file-oriented.**
   Single-file `scpp foo.scpp` remains valid, but large-project build must think in packages/workspaces/profiles.

2. **Manifest-based project mode; manifestless non-project mode.**
   Zero-config single-file use stays valid, but package/workspace builds are manifest-based.

3. **Integrated public UX, not generator-first UX.**
   The user should run `scpp build`, not `scpp gen && ninja`.

4. **Artifacts are the build graph boundary.**
   `.scppm` is the compile-time dependency boundary; `.scppa` is the link-time artifact boundary; `.scppkg` is the distribution boundary.

5. **Direct dependencies are the compile-time contract.**
   Transitive packages may be linked transitively, but imported modules should come only from the current package or direct dependencies.

6. **No programmable build scripts in v1.**
   Declarative manifests only.

7. **Do not invent features/conditional compilation in the build system before the language has a coherent story for it.**

## 4. Recommended top-level design

## 4.1 Public model

Introduce an **optional manifest** named `scpp.toml`.

- If `scpp.toml` is present, the directory is a manifest-based package or workspace.
- If `scpp.toml` is absent, the user is in **non-project mode** instead.

This keeps project-scale behavior explicit without disturbing scpp's existing direct-compiler feel for non-project use.

## 4.2 Execution model

Expose a **Cargo-like integrated command set**:

- `scpp build`
- `scpp run`
- `scpp test`
- `scpp package`
- `scpp clean`

with bare `scpp` as a convenience alias for "build the default thing in this directory".

## 4.3 Internal engine choice

**Decision: use an integrated planner/executor in v1, not a mandatory Ninja/CMake-style generator.**

Why:

- scpp's build graph is scpp-specific: modules, partitions, `.scppm`, `.scppa`, generics blocks, package manifests;
- requiring Ninja or another backend would complicate the first user experience;
- Cargo proves that an integrated tool can scale well enough for large real projects;
- the artifact split already gives scpp a natural incremental boundary that Cargo does not have.

V1 does **not** need a backend-abstraction layer or a generated-backend escape hatch. If that need becomes real later, it can be added later as its own command (`scpp gen --backend ...`) instead of shaping the v1 architecture around a capability the user does not want to require yet.

## 5. Project/package model

## 5.1 Package vs module

The build system should make the same distinction the existing `.scppkg` format already makes:

- a **package** is the unit of dependency declaration and workspace membership;
- a **module** is the unit of import/export and `.scppm` generation.

A single package may provide:

- zero or one library target;
- zero or more binary targets;
- one or more modules inside those targets.

This matches Cargo/Go package thinking and also matches scpp's existing package format.

## 5.2 Manifestless non-project mode

Absence of `scpp.toml` should be understood as **non-project mode**, not as an alternate fully-fledged project model.

Today's non-project mode is the existing single-file CLI behavior:

- `scpp <file.scpp> ...` compiles a single file directly.[^cli-current]

The paused `main.scpp` / `lib.scpp` branch remains useful context as the closest thing scpp has explored for a directory-scale manifestless convenience, but this design does **not** broaden that into the main v1 project story, and specifically does **not** add `src/main.scpp` / `src/lib.scpp` recognition. If the `main.scpp` / `lib.scpp` convenience is retained at all, it should be documented as a narrow non-project convenience rather than as general project mode.[^project-mode-branch]

Whether scpp later grows a stricter multi-file manifestless mode is explicitly deferred; for now, package/workspace builds are manifest-based.

## 5.3 Manifest-based mode

Recommended manifest file: `scpp.toml`

Example:

```toml
manifest-version = 1

[package]
name = "httpserver"
version = "0.1.0"          # required for packaging, recommended otherwise

[[lib]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[[bin]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[dependencies]
net = { path = "../net" }
json = { scppkg = "vendor/json.scppkg" }

[profile.dev]
opt-level = 0
debug = true
static = false

[profile.release]
opt-level = 3
debug = false
static = true

[native]
links = ["pthread"]
search = ["native/lib"]
```

### Why TOML?

Because Cargo has already demonstrated that TOML is readable enough for hand-editing, structured enough for tools, and far less ceremony-heavy than XML or a new DSL.[^cargo-manifest]

### Why not `CMakeLists.txt`-style DSL?

Because scpp does not need a second language for project metadata. Declarative tables cover the real v1 needs better.

## 5.4 Manifest fields

Recommended initial fields:

- `manifest-version` — schema version for the manifest format.
- `[package]`
  - `name` (required)
  - `version` (required for `scpp package`, optional-but-recommended otherwise)
- `[[lib]]`
  - `name`
  - `sources`
  - optional `additional_objs`
- `[[bin]]`
  - `name`
  - `sources`
  - optional `additional_objs`
- `[dependencies]`
- `[profile.dev]`, `[profile.release]`, optional custom `[profile.<name>]`
- `[native]` for package-wide native link requirements
- `[additional_objs.<name>]` for a named custom build step that can produce additional objects
- `[package.metadata]` reserved for external tools

Recommended v1 restriction: **at most one library target per package**.

If a repository needs many independently reusable libraries, it should use a workspace with many packages instead of many library targets inside one package. That keeps packaging and dependency resolution much simpler.

## 6. Workspace model

Adopt a Cargo-like workspace root.

Example root manifest:

```toml
manifest-version = 1

[workspace]
members = [
  "libs/net",
  "libs/json",
  "apps/httpserver",
]
default-members = ["apps/httpserver"]

[workspace.dependencies]
net = { path = "libs/net" }
json = { path = "libs/json" }

[profile.dev]
opt-level = 0
debug = true

[profile.release]
opt-level = 3
debug = false
```

Member manifest:

```toml
manifest-version = 1

[package]
name = "httpserver"
version = "0.1.0"

[[bin]]
name = "httpserver"
sources = ["src/**/*.scpp"]

[dependencies]
net = { workspace = true }
json = { workspace = true }
```

### Workspace rules

Recommended rules:

- a workspace root may be either:
  - a **virtual workspace** (`[workspace]` only), or
  - a **root package workspace** (`[workspace]` + `[package]`), mirroring Cargo.[^cargo-workspaces]
- `scpp build` from workspace root builds `default-members`; `scpp build --workspace` builds every member.
- profiles defined at the workspace root override member-local profile tables, mirroring Cargo's root-owned profiles.[^cargo-workspaces][^cargo-profiles]
- workspace root owns the shared build/cache output directory.

### Why workspaces instead of CMake `add_subdirectory()`?

Because workspaces model **packages with identity**, not merely directories with imperative script inclusion. That is a better fit for scpp's future `.scppkg` ecosystem.

## 7. Dependency model

## 7.1 V1 dependency kinds

Recommended v1 dependency sources:

1. `path = "../foo"`
   - a local package directory containing `scpp.toml`
2. `scppkg = "vendor/foo.scppkg"`
   - a packaged dependency file

Reserve, but do not implement yet:

- `git = ...`
- `version = ...` with a registry source

This avoids painting the design into a corner while keeping v1 aligned with what actually exists today.

## 7.2 Compile-time visibility rule

**Recommendation: only direct dependencies are visible for module resolution.**

If package `app` depends on package `net`, and `net` depends on `tls`, then:

- `app` may import modules exported by `net`;
- `app` may **not** import modules exported only by `tls` unless `tls` is also declared directly in `app`'s `[dependencies]`.

Why:

- matches Cargo/Bazel expectations;
- keeps the package manifest honest;
- prevents hidden coupling to transitive package structure.

## 7.3 Link-time closure rule

Compile-time visibility is direct-only, but final linking should still use the **transitive dependency closure**, consistent with the existing scpp language spec's linking model.[^modules-linking]

That means:

- imported modules must resolve through direct dependencies;
- linked `.scppa` archives and native link requirements propagate transitively.

This is exactly the place where CMake's transitive usage-requirements idea is valuable.[^cmake-target-link]

## 7.4 Future lockfile

I recommend defining the file name **now**: `scpp.lock`.

However, I do **not** recommend making it the central v1 feature.

Rationale:

- Cargo's lockfile is highly valuable once registries/git dependencies exist.[^cargo-lock]
- in a path-only ecosystem, a lockfile cannot guarantee reproducibility in the strong sense, because local paths are mutable;
- forcing heavy lock semantics too early would add churn without delivering equivalent value.

So the recommended path is:

- reserve `scpp.lock` from day one;
- reserve a future dependency-resolution/fetch step, best surfaced as `scpp pull`, for the point where registry/git dependencies exist;
- allow `scpp package` / future registry work to populate and consume it later;
- keep v1 local-workspace builds functional without depending on it.

User review confirmed this exact direction: `scpp.lock` should be reserved now, but not made central for path-only v1 builds.

## 8. Source discovery and target graph construction

## 8.1 Manifest-declared source sets

Each target (`[[lib]]`, `[[bin]]`) should declare:

- one `name` field (`[[lib]]` artifact name or `[[bin]]` executable name);
- one `sources` glob set.
- optionally one `additional_objs` reference naming one or more `[additional_objs.<name>]` steps.

Each `[additional_objs.<name>]` block declares:

- `input` — files whose digests participate in rebuild decisions;
- `output` — files the command must produce;
- `command` — one shell command, run once, whose outputs are then fed into the
  target's final archive/link step.

The build tool then parses the listed source files to discover:

- primary interface units (`export module foo;`)
- implementation units (`module foo;`)
- interface partitions (`export module foo:bar;`)
- implementation partitions (`module foo:bar;`)
- plain non-module source files.

This avoids forcing users to manually duplicate the module graph in the manifest.

## 8.2 Grouping rule

Within one target's source set:

- all units declaring the same module name belong to the same logical module build;
- exactly one primary interface unit is required per module;
- implementation units and partitions attach to that module;
- plain non-module files become ordinary compilation units for the enclosing target.

This is simply the project-build generalization of the existing chapter-11 rules.[^modules-book]

## 8.3 Cross-package module resolution

During planning, the build tool constructs a table:

- module name -> producing package + producing target + expected `.scppm` path

Resolution order:

1. current target/package modules
2. direct dependency packages' exported modules
3. stdlib built-ins / shipped package roots
4. explicit overrides if the CLI later adds them for project mode

Ambiguity should be a hard error:

- if two direct dependencies both export the same module name, the user must disambiguate by changing dependencies/package choices, not by import-site aliasing.

That matches scpp's current language preference for avoiding hidden lookup magic.

## 9. Artifact model and `.scppm` / `.scppa` / `.scppkg` integration

## 9.1 Key observation

scpp's existing artifact split is already the right incremental boundary:

- `.scppm` changes mean **compile-time interface change**;
- `.scppa` changes without `.scppm` changes mean **relink needed, but downstream recompilation may be avoidable**.

This is a major advantage over header-style systems.

## 9.2 Local build outputs

Recommended workspace-local output root:

```text
.scpp/
  build/
    <triple>/
      <profile>/
        <package>/
          modules/
            foo.scppm
            bar.scppm
          archives/
            libfoo.scppa
            libbar.scppa
          objects/
          package-metadata.json
  cache/
    build.db
```

Notes:

- `.scpp/` is intentionally project-local and not for distribution.
- `package-metadata.json` is an internal build artifact, not a published format.
- the internal metadata should be shaped to map cleanly onto `.scppkg`'s `MANIFEST.json`, so local and packaged dependencies share one conceptual model.

## 9.3 Package build outputs

A library package build should produce:

- one `.scppm` per exported module;
- one `.scppa` per compiled module and target triple;
- metadata mapping package -> exported modules -> artifact paths -> native link requirements.

A binary package build should produce:

- ordinary object files for plain sources and module-local compiled code;
- a final linked executable.

## 9.4 Packaging command

Introduce:

```console
scpp package
```

Behavior:

- valid only for packages with `[[lib]]`;
- builds the package in the selected profile/target;
- bundles the produced `.scppm` / `.scppa` artifacts and manifest metadata into a `.scppkg` file following the existing spec.[^scppkg-spec]

Suggested default output location:

```text
dist/<package-name>-<version>-<target-triple>.scppkg
```

The `.scppkg` spec already supports multiple target triples per module; v1 packaging may start with one triple per invocation and extend later.

## 9.5 Native link requirements in packaged output

The `.scppkg` spec already records `native_link_requirements` per module.[^scppkg-spec]

For v1, target-level/package-level native requirements can be lowered into that schema conservatively:

- if a library target declares package-wide `links = ["ssl", "crypto"]`, each binary module emitted from that target may carry the same requirement set in the produced package manifest.

That is slightly coarse, but correct, and can be refined later if per-module declarations become necessary.

## 10. Profiles and configuration

## 10.1 Built-in profiles

Adopt built-in profiles:

- `dev`
- `release`

with optional custom profiles, mirroring Cargo.[^cargo-profiles]

Recommended defaults:

### `dev`

- `opt-level = 0`
- `debug = true`
- `static = false`
- incremental rebuilds enabled

### `release`

- `opt-level = 3`
- `debug = false`
- `static = false` by default, overridable per project
- more aggressive codegen / LTO hooks later

## 10.2 CLI/profile interaction

Recommended commands:

- `scpp build` => `dev`
- `scpp build --release` => `release`
- `scpp build --profile release-lto` => custom named profile
- `scpp run` => builds with `dev` unless overridden

Existing one-off flags should map cleanly:

- existing single-file `-g` concept maps to `debug = true`
- existing `--static` concept maps to `static = true`

For project mode, these should become **profile properties first**, CLI overrides second.

## 10.3 Why no Cargo-style features in v1?

Because Cargo features are tightly tied to Rust conditional compilation and optional dependency activation.[^cargo-features]

scpp explicitly has no preprocessor, and the language has no settled conditional-compilation feature system today.[^modules-book]

So adding a build-system feature matrix now would create pressure for hidden language semantics that do not exist yet.

## 11. Native / foreign dependency story

## 11.1 V1 recommendation

V1 should support a **declarative manual** native dependency model in the manifest.

Recommended fields:

```toml
[native]
links = ["m", "pthread"]
search = ["native/lib", "/opt/foo/lib"]
```

Later-expandable fields:

```toml
frameworks = ["Security"]
discover = ["openssl"]
```

## 11.2 Why manual first?

Because it is enough to replace today's raw `--link path` world with something project-scale and version-controlled, without immediately forcing scpp to become a cross-platform system package discovery framework.

Meson's example is still useful as evidence that **declarative dependency descriptions are better than raw flag soup**.[^meson-deps] But user review clarified an important scpp-specific constraint: the later evolution should be a **scpp-native discovery mechanism**, not a long-term plan to depend on `pkg-config`, CMake package discovery, or other third-party discovery tools.

## 11.3 Propagation rule

Native link requirements should propagate transitively like CMake `PUBLIC` usage requirements:[^cmake-target-link]

- if package `net` depends on native `ssl` and `crypto`, any final executable depending on `net` should inherit those link requirements automatically.

## 12. CLI surface

## 12.1 Keep existing low-level commands

Retain:

- `scpp <file.scpp> ...`
- `scpp build-module ...`
- `scpp lex ...`
- `scpp parse ...`

`build-module` remains the expert / plumbing command for one module's artifacts.

## 12.2 Add project-scale commands

Recommended new commands:

```console
scpp build [--workspace] [-p <package>] [--bin <name>] [--lib] [--profile <name>] [--release]
scpp run   [--workspace] [-p <package>] [--bin <name>] [--profile <name>] [--release] [-- <program args>]
scpp test  [--workspace] [-p <package>] [--profile <name>] [--release] [-- <test args>]
scpp package [-p <package>] [--profile <name>] [--release]
scpp clean
```

Package-selection flags intentionally mirror Cargo because they are proven and unsurprising in multi-package repositories.[^cargo-workspaces]

`scpp test` should be named now as a first-class project command, but its actual mechanics are **not** fully designable yet because scpp does not currently have a language-level test function/attribute/discovery model analogous to Rust's `#[test]` plus `cargo test`. In other words, the command name should exist in the build-system roadmap now, while its eventual execution model likely depends on a later language/tooling design for project-defined test targets (for example, manifest-declared test targets and/or a conventional `tests/` layout once the language has a coherent way to mark runnable tests).

Separately, the future escape hatch for generating build files for another backend should be surfaced as an explicit subcommand:

```console
scpp gen --backend ninja
```

rather than only as a flag hanging off `scpp build`. User review also clarified that `scpp gen` is **not** a v1 requirement; it is a later addition only.

## 12.3 Bare `scpp`

Recommended behavior:

- if given a `.scpp` file argument => current single-file behavior
- if given no file argument and a project/workspace is detected => behave as `scpp build` and build the default target(s)
- if given no file argument and no project is detected => current help/error path

This preserves the session's zero-ceremony direction without making project mode ambiguous.

There is also a forward-looking reason to make that choice explicit now: once scpp eventually gains real registry / git dependency resolution, bare `scpp` can naturally be understood as **`scpp pull` + `scpp build`** -- first resolve/fetch external dependencies, then build. Because no such pull/registry mechanism exists today, bare `scpp` should simply mean `scpp build` for now. That keeps today's UX simple while leaving a clean conceptual path for future external-dependency workflows.

This rule applies specifically when a manifest-based project/workspace has been detected. Absence of a manifest remains non-project mode as described in §5.2.

## 13. Incremental and parallel build strategy

## 13.1 Why scpp's artifact split makes this tractable

The build system can use two distinct invalidation boundaries:

- **compile invalidation**: any upstream `.scppm` change
- **link invalidation**: any upstream `.scppa` change, even if `.scppm` stayed the same

That means:

- changing only implementation code inside a dependency module can often avoid downstream recompilation;
- downstream binaries still relink against the new `.scppa`.

This is one of the design's strongest benefits and should be exploited explicitly.

## 13.2 Build database

Recommended internal state:

- a local SQLite build database (for example `.scpp/cache/build.db`)
- one row per compiled module / plain source / binary target / package artifact
- cached keys include:
  - source file digests or mtimes + sizes
  - active profile
  - target triple
  - compiler version
  - resolved dependency artifact digests/paths
  - manifest digest

## 13.3 Scheduler

Recommended v1 scheduler behavior:

- scan manifests and sources;
- construct a package graph;
- inside each package, construct a module/import DAG;
- schedule ready modules in parallel;
- schedule plain-source compilation after required local/dependency `.scppm` artifacts exist;
- link targets once all object/archive inputs are ready.

This is effectively Cargo's integrated execution model, but with scpp-specific module/interface boundaries.

## 13.4 Why not mandatory Ninja generation in v1?

Because the difficult part for scpp is the **graph semantics**, not the shelling-out mechanics.

Once scpp already knows:

- which modules exist,
- which packages export them,
- which `.scppm` changes require recompilation,
- which `.scppa` changes require relinking,

then executing the graph directly is a reasonable first implementation.

A later `scpp gen --backend ninja` style escape hatch can be added if it proves useful for IDEs, debugging, or external build-tool integration, but v1 should not be burdened with a backend-abstraction requirement ahead of that real need.

## 14. Alternatives considered

## 14.1 Pure filesystem convention, no manifest ever

Rejected as the main solution.

Why:

- cannot express dependencies/workspaces/profiles/native link metadata cleanly;
- cannot support publishable package identity without additional files anyway;
- would force increasingly magical heuristics as projects scale.

Keep it only as the non-project fallback.

## 14.2 Mandatory manifest for every project, even tiny ones

Rejected.

Why:

- fights the current `scpp foo.scpp` / zero-flag direction;
- makes the trivial case worse for little gain.

## 14.3 CMake-style generator as the primary model

Rejected for v1 public UX.

Why:

- too much ceremony for a single-language toolchain that already owns compilation semantics;
- would make scpp project builds feel like "write build scripts for another build tool" instead of "use scpp to build scpp";
- generator export is useful as a later optional feature, not as the core mental model.

## 14.4 Build-system-as-scpp-code (`build.scpp`, Zig-style)

Rejected for v1.

Why:

- bootstrapping and host-execution complexity;
- security/determinism concerns;
- overpowered relative to what the ecosystem needs right now.

## 15. Recommended phased implementation plan

## Phase A — land the project model skeleton

1. add project/workspace discovery (`scpp.toml` search upward);
2. make manifest presence the boundary for project mode vs non-project mode;
3. add `scpp build` as the explicit project-scale command;
4. make bare `scpp` behave as `scpp build` in project/workspace mode.

## Phase B — manifest parser + single-package builds

1. parse `scpp.toml`
2. support `[package]`, `[[lib]]`, `[[bin]]`, `[profile.*]`
3. build one manifest-based package with no external dependencies
4. write local outputs under `.scpp/build/...`

## Phase C — path dependencies + workspaces

1. add `[dependencies] path = ...`
2. add `[workspace]`, `members`, `default-members`
3. add `-p/--package`, `--workspace`
4. share build output/cache at workspace root

## Phase D — incremental graph + native metadata

1. add build database
2. distinguish compile vs link invalidation using `.scppm` vs `.scppa`
3. add declarative `[native]` propagation
4. parallelize ready nodes

## Phase E — packaging / `.scppkg`

1. add `scpp package`
2. consume `scppkg = ...` dependencies
3. map local package metadata to `.scppkg` `MANIFEST.json`
4. reserve `scpp.lock` semantics for future non-path dependency resolution

## Phase F — optional later enhancements

- a scpp-native native-dependency discovery mechanism layered above declarative metadata
- `scpp pull` for registry/git dependency resolution once such dependency sources exist
- `scpp gen --backend <name>` for generating external-backend build files (for example Ninja)
- `scpp metadata` JSON for IDE tooling
- git/version/registry dependencies
- `scpp test`, once scpp has a coherent language/tooling story for project-defined tests and test discovery
- tests/examples/bench target conventions once the ecosystem has clearer conventions

## 16. Final settled design decisions

User review has now resolved the remaining forks. The final design decisions are:

1. **Use `scpp.toml` as the boundary for package/workspace project mode.**
2. **Treat absence of a manifest as non-project mode; today that means single-file use, with any retained `main.scpp` / `lib.scpp` convenience staying narrow and manifestless.**
3. **Make the public build UX integrated and Cargo-like (`scpp build`, `scpp run`, `scpp package`, later `scpp test`), not generator-first.**
4. **Make bare `scpp` mean `scpp build` when a manifest-based project/workspace is detected; later, once external resolution exists, that can grow conceptually into `scpp pull` + `scpp build`.**
5. **Use packages/workspaces as the dependency model; use modules as the compilation artifact model.**
6. **Allow only direct dependencies for compile-time module visibility.**
7. **Propagate `.scppa` and native link requirements transitively for final linking.**
8. **Do not add programmable build scripts in v1.**
9. **Do not add Cargo-style features / conditional dependency activation in v1.**
10. **Reserve `scpp.lock` now, but do not make it central in path-only v1 builds.**
11. **Keep native dependencies manual in v1, and plan for a future scpp-native discovery mechanism rather than `pkg-config` / CMake-based discovery providers.**
12. **Do not require `scpp gen` or backend abstraction in v1; add generated-backend support later only if real need emerges.**
13. **Require package `version` for `scpp package`, but keep it optional for local-only builds.**
14. **Exploit `.scppm` vs `.scppa` as separate incremental invalidation boundaries.**

## Appendix A — empirical findings worth preserving

### Cargo

Local check performed in this workspace:

```console
$ cargo build --workspace
   Compiling lib v0.1.0 (.../lib)
   Compiling app v0.1.0 (.../app)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.43s
```

Observed results:

- one workspace-root `Cargo.lock`
- one shared workspace-root `target/`
- default build used Cargo's `dev` profile

### CMake

Local check performed in this workspace:

```console
$ cmake -S . -B build
-- Configuring done
-- Generating done
-- Build files have been written to: .../build

$ cmake --build build
[100%] Built target app
```

Observed result:

- CMake's public model is clearly configure/generate first, build second.

### scpp reference repo

The reference repo's own stdlib CMake currently drives `scpp build-module` to produce:

- `std.scppm`
- `libstd.scppa`

from the stdlib's module sources, which is exactly the kind of artifact-oriented project build this design generalizes.[^stdlib-cmake]

## Sources

[^modules-book]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md`, especially module/package/artifact sections and §11.12-§11.15.
[^scppm-spec]: `scpp-reference/docs/spec/en/scppm-format.md`.
[^scppkg-spec]: `scpp-reference/docs/spec/en/scppkg-format.md`.
[^modules-out-of-scope]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md` §11.15.
[^cli-current]: `scpp-reference/src/cli.cppm:603-691`.
[^project-mode-branch]: `scpp-reference` remote branch `origin/dev-agent/project-build-mode`, especially `src/cli.cppm:669-742` and related tests/docs discovered via `git grep`.
[^modules-linking]: `scpp-reference/docs/book/en/ch11-modules-and-libraries.md:464-522`.
[^stdlib-cmake]: `scpp-reference/stdlib/CMakeLists.txt:14-35`.
[^cargo-manifest]: Cargo Book, "The Manifest Format" — https://doc.rust-lang.org/cargo/reference/manifest.html
[^cargo-deps]: Cargo Book, "Specifying Dependencies" — https://doc.rust-lang.org/cargo/reference/specifying-dependencies.html
[^cargo-workspaces]: Cargo Book, "Workspaces" — https://doc.rust-lang.org/cargo/reference/workspaces.html
[^cargo-profiles]: Cargo Book, "Profiles" — https://doc.rust-lang.org/cargo/reference/profiles.html
[^cargo-lock]: Cargo Book, "Cargo.toml vs Cargo.lock" — https://doc.rust-lang.org/cargo/guide/cargo-toml-vs-cargo-lock.html
[^cargo-build-scripts]: Cargo Book, "Build Scripts" — https://doc.rust-lang.org/cargo/reference/build-scripts.html
[^cargo-features]: Cargo Book, "Features" — https://doc.rust-lang.org/cargo/reference/features.html
[^cmake-manual]: CMake manual `cmake(1)` — https://cmake.org/cmake/help/latest/manual/cmake.1.html
[^cmake-add-subdirectory]: CMake `add_subdirectory()` — https://cmake.org/cmake/help/latest/command/add_subdirectory.html
[^cmake-target-link]: CMake `target_link_libraries()` and `cmake-buildsystem(7)` — https://cmake.org/cmake/help/latest/command/target_link_libraries.html and https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html
[^cmake-find-package]: CMake `find_package()` — https://cmake.org/cmake/help/latest/command/find_package.html
[^cmake-presets]: CMake `cmake-presets(7)` — https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
[^bazel-overview]: Bazel external dependency overview — https://bazel.build/external/overview
[^go-mod-ref]: Go Modules Reference — https://go.dev/ref/mod
[^go-managing-deps]: Go, "Managing dependencies" — https://go.dev/doc/modules/managing-dependencies
[^zig-build]: Zig, "Zig Build System" — https://ziglang.org/learn/build-system/
[^meson-deps]: Meson, "Dependencies" — https://mesonbuild.com/Dependencies.html
[^local-cargo-check]: local command run in `~/scpp-agents/build-system-designer-agent/scratch-research` during this session.
[^local-cmake-check]: local command run in `~/scpp-agents/build-system-designer-agent/scratch-cmake` during this session.
