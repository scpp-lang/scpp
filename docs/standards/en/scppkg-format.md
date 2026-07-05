# The `.scppkg` Package Format

This document specifies `.scppkg`: a package bundling one or more scpp
modules for distribution -- as raw `.scpp` interface source, or as a
`.scppm` interface (see
[The `.scppm` Module Interface Format](scppm-format.md)) paired with one
`.scppa` archive per target triple, or a mix -- into one distributable
archive.

`.scppkg` is referenced from
[ch11 §11.12](../../book/en/ch11-modules-and-libraries.md#1112-the-scppm-scppa-and-scppkg-formats)
of the language specification.

A `.scppa` file, named from a `.scppkg` manifest (§3), is not a format
this document defines: it is exactly the target platform's own native
static-library archive format (the `ar` format for a Unix `.a`, or a
Windows `.lib`), bundling one `.scppo` object member per file that
contributed code to a module -- its primary interface unit, an
implementation unit, or a partition (see ch11
[§11.3](../../book/en/ch11-modules-and-libraries.md#113-export-surface-and-the-interfaceimplementation-split)
and
[§11.4](../../book/en/ch11-modules-and-libraries.md#114-module-partitions))
-- for one target triple. Because it is a native archive, a system linker
reads a `.scppa` file directly and unpacks it exactly as it would any
other static library; nothing about it is scpp-specific except which
`.scppo` members happen to be inside, and members need not even be named
`.scppo` internally for that to work (a linker locates symbols via the
archive's own native index, not member names).

A `.scppo` file, in turn, is also not a format this document defines: it
is exactly the target platform's own native object file format (ELF,
COFF, Mach-O, etc.).

## 1. Envelope

A `.scppkg` file wraps a
[tar](https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html)
archive in an envelope:

```
[Header]           -- fixed size, 8 bytes, always stored raw
[payload_length]   -- uint64, little-endian, 8 bytes
[Payload]          -- payload_length bytes: a tar archive, raw or a single LZMA stream, per the header's flags
[Signature block]  -- optional, trailing, present only if flags bit 4 is set (§4)
```

### Header

A fixed 8-byte header, never compressed:

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 5 | `magic` | ASCII bytes `SCPPK`. |
| 5 | 1 | `major_version` | uint8. This document defines major version `1`. |
| 6 | 1 | `patch_version` | uint8. This document defines patch version `0`. |
| 7 | 1 | `flags` | Bits 3-0, bit 4, bits 5-7 (below). |

Bits of `flags`:

| Bits | Meaning |
|---|---|
| 3-0 | The payload's compression scheme: `0` = stored raw; `1` = a single LZMA stream (§2). |
| 4 | Trailing signature block (§4) present. `0` in this version: no such block is present. |
| 7-5 | Reserved, `0` in this version. |

A reader checks `magic` and `major_version` before parsing anything else.
An unrecognized `magic`, or a `major_version` the reader does not support,
is a hard parse failure reported at this point -- never a crash or a
best-effort guess further into the file. A `patch_version` newer than the
reader knows about, and any `flags` bit the reader does not recognize the
meaning of, are not errors: both only ever signal an optional, ignorable
addition -- a reader ignores whatever it does not recognize and proceeds.

### Payload

Immediately following the header:

| Field | Size | Value |
|---|---|---|
| `payload_length` | 8 bytes, uint64 LE | Byte length of the payload that follows, as stored (its LZMA-compressed size if `flags` selects lzma, or its raw byte length otherwise). |
| Payload | `payload_length` bytes | Raw tar bytes if `flags` selects none; a single LZMA stream (§2) if `flags` selects lzma. Decoding it yields a tar archive, of the shape given in §3. |

A reader that only wants the tar archive reads exactly `payload_length`
bytes at a fixed position (right after the header and `payload_length`
field) and never needs to read further -- regardless of whether a trailing
signature block (§4) is present, since that block always starts exactly at
`payload_length` bytes after the payload begins, a position the reader
already knows without inspecting anything past the payload itself.

## 2. LZMA stream format

Where `flags` (§1) selects lzma, the payload's stored bytes are a raw LZMA
stream: a 5-byte properties header (`lc`/`lp`/`pb` packed into byte 0,
followed by a 4-byte little-endian dictionary size), followed by an 8-byte
little-endian uncompressed-size field giving the decoded tar archive's byte
length, followed by the LZMA-compressed data. This is exactly what the
public-domain LZMA SDK's simple encoder (`LzmaCompress`) produces. No
`.xz`/`.7z` framing or other container is layered on top.

## 3. `.scppkg` contents

Decoding the payload (§1) yields a tar archive containing:

| Path | Contents |
|---|---|
| `MANIFEST.json` | This package's manifest (below). |
| any path a module's `interface` names | A nested, complete `.scppm` file (see [The `.scppm` Module Interface Format](scppm-format.md)). |
| any path a module's `archives[].path` names | A `.scppa` file: this module's native archive of compiled `.scppo` objects for one target triple. |
| any path a module's `path` names | A raw `.scpp` interface source file (`kind: "source"` modules only). |

`MANIFEST.json`:

```json
{
  "schema_version": "1.0",
  "package": { "name": "mylib", "version": "1.2.0" },
  "dependencies": [
    { "name": "otherlib", "version": "^1.0.0" }
  ],
  "modules": {
    "mylib.math": {
      "kind": "binary",
      "interface": "mylib.math.scppm",
      "archives": [
        { "target_triple": "x86_64-linux-gnu",
          "path": "mylib.math.x86_64-linux-gnu.scppa" }
      ],
      "native_link_requirements": ["m"]
    },
    "mylib.collections": { "kind": "source", "path": "mylib.collections.scpp" }
  }
}
```

| Field | Meaning |
|---|---|
| `schema_version` | This manifest schema's own `"major.minor"` version (§4), independent of the envelope's `major_version`/`patch_version` (§1) and of any nested `.scppm`'s own version. This document defines `"1.0"`. Read and validated before any other manifest key. |
| `package.name` | This package's name. Freeform. |
| `package.version` | This package's version. Freeform. |
| `dependencies` | List of other packages this package needs (below). Author-provided package-management metadata; this document does not specify how `version` is matched or resolved. |
| `modules.<dotted-module-name>.kind` | `"binary"` (this entry has `interface` and `archives`) or `"source"` (this entry has `path`). |
| `modules.<dotted-module-name>.interface` | Present only if `kind` is `"binary"`. Tar-internal path of the nested `.scppm` file. |
| `modules.<dotted-module-name>.archives` | Present only if `kind` is `"binary"`. List of this module's `.scppa` files, one per target triple (below). |
| `modules.<dotted-module-name>.path` | Present only if `kind` is `"source"`. Tar-internal path of the raw `.scpp` interface source file. |
| `modules.<dotted-module-name>.native_link_requirements` | System libraries (`-l`-style names) this module's compiled code depends on at link time. Author-provided; absent if none. |

Each `dependencies` entry:

| Field | Meaning |
|---|---|
| `name` | The other package's name. |
| `version` | The other package's version requirement. Opaque to this document. |

Each `archives` entry:

| Field | Meaning |
|---|---|
| `target_triple` | The target triple this `.scppa` file was built for. |
| `path` | Tar-internal path of the `.scppa` file. |

A module built from several contributing files (its primary interface
unit, one or more implementation units, one or more partitions -- see
ch11 [§11.3](../../book/en/ch11-modules-and-libraries.md#113-export-surface-and-the-interfaceimplementation-split)
and
[§11.4](../../book/en/ch11-modules-and-libraries.md#114-module-partitions))
still has exactly one `archives` entry per target triple: the `.scppa`
file itself is a native archive bundling however many `.scppo` members
that module needed for that triple, so this manifest never has to
enumerate them individually.

`dependencies` and `native_link_requirements` are package-management
metadata, not language-level facts: a `.scppm` file carries neither, since
a real C++ module has no equivalent of either. `dependencies` is a single,
package-wide list -- consistent with how other package ecosystems resolve
dependencies at package granularity, not per file or module -- while
`native_link_requirements` is recorded per module, since different
modules' compiled code may need different system libraries.

A package may provide the same module as both `kind: "source"` and (under a
different entry) as `kind: "binary"` for a consumer to choose between; this
document does not require the two, if both present, to agree on which
module names exist elsewhere in the package.

## 4. Extensibility

`schema_version` (§3) is a `"major.minor"` string, versioning the manifest
schema independently of the envelope's own `major_version`/`patch_version`
(§1). A reader checks its major component before trusting the rest of the
manifest; a major component the reader does not support is a hard parse
failure, reported before any other manifest key is read. A minor
component newer than the reader knows about is not an error -- it only
signals optional fields the reader does not recognize, which it ignores
before proceeding. New optional `MANIFEST.json` fields are introducible as
a `schema_version` minor-version bump; a breaking change to the schema --
removing a field, or repurposing an existing one -- requires a
major-version bump instead.

The envelope's own `major_version`/`patch_version` (§1) version the byte
container itself, independently of `schema_version`: they govern the
header/payload/signature framing described in §1, not the manifest schema
nested inside the payload.

A signature over the envelope's header and payload together (so that
neither can be altered independently of the other) is not embedded inside
the payload's own tar archive. It is a trailing block, immediately
following the payload and present only when `flags` bit 4 (§1) is set:

| Field | Size | Value |
|---|---|---|
| `signature_length` | 8 bytes, uint64 LE | Byte length of the signature that follows. |
| Signature | `signature_length` bytes | Opaque to this document; covers the header and payload bytes together. |

Bit 4 is `0` in this version: no such block is present, and the file ends
immediately after the payload. Because a reader locates the end of the
payload from `payload_length` (§1) alone, a reader that predates this block
still correctly reads the payload regardless of whether a signature block
follows it.

Neither the signature's own format, nor how a verifier establishes which
key or certificate to trust, is specified by this document.
