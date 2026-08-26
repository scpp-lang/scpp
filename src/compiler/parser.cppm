module;

export module scpp.parser;

import std;
import scpp.lexer;
import scpp.ast;

export namespace scpp {

class ParseError : public std::runtime_error {
public:
    ParseError(int line, int column, const std::string& message)
        : runtime_error{message}, line{line}, column{column}, loc{make_source_location(line, column)} {}

    // Explicit copy constructor: runtime_error itself declares no copy
    // constructor of its own (see std_stdexcept.scpp), so it must be
    // reconstructed here from what() rather than copy-initialized
    // directly. Needed so a fresh ParseError can convert into a
    // std::expected<T, ParseError> via std::expected's copy-based
    // std::unexpected<E> converting constructor (std_expected.scpp) --
    // the only route available, since scpp has no lightweight way to
    // move a value out of std::unexpected<E>'s private storage across
    // module/class boundaries (see std_expected.scpp's history).
    ParseError(const ParseError& other)
        : runtime_error{std::string{other.what()}}, line{other.line}, column{other.column}, loc{other.loc} {}

    virtual ~ParseError() override = default;

    int line;
    int column;
    // Same position as line/column above, just packaged as a
    // SourceLocation (ast.cppm) so cli.cppm's diagnostic printer can
    // treat every error kind (Parse/Dataflow/Codegen) uniformly. Built
    // from a bare line/column pair above -- loc.source_path starts unset
    // -- since plumbing this parser instance's own source_path_ through
    // every one of this file's 150+ `return std::unexpected(ParseError(...))`
    // sites isn't practical; the free parse() function at the bottom of
    // this file stamps it in once, at the parse()/Parser boundary,
    // instead (see its own comment).
    SourceLocation loc;
};

// ch11 §11.8: given a module's dotted name (e.g. "std"), returns a
// pointer to that module's already-parsed (and, transitively, already
// import-resolved) Program -- called while parsing an `import name;`
// declaration, so the imported module's exported struct/class names are
// registered (struct_names_/class_names_) before the rest of the
// importing file is parsed (mirrors real C++20: imports must precede
// every other declaration). Owned and cached by the driver (which knows
// about `--import name=path` mappings and file I/O -- the parser itself
// never touches the filesystem). Returns std::expected rather than a
// bare pointer so the driver can report a resolution failure (module
// not found, circular import, etc.) back through the same channel this
// file already uses for every other error -- previously this had to
// throw instead, since a bare `const Program*` return type has no
// disengaged state of its own to report failure through; see
// parse_import_declarations for how a disengaged result here is told
// apart from a propagated, already-positioned ParseError coming back
// out of a *nested* parse (this callback's own ParseError::loc.is_known()
// is the signal: unknown means "resolver-native failure, no position of
// its own yet", known means "forward verbatim, it already points at the
// real problem inside the imported file"). A successful call is still
// never expected to return a null pointer in practice -- this file still
// treats a null value as a (defensively-checked, not merely asserted)
// resolution failure rather than trusting that invariant blindly (see
// parse_import_declarations). A pointer, rather than the reference this
// alias used before, is required here: scpp's own lifetime checker
// (self-hosting this file's own compiler) has no mechanism yet for a
// stored/type-erased callable (this alias's own std::function,
// ultimately) to carry a lifetime-group relationship tying its return to
// any of its arguments or captured state across an indirect call -- see
// std::function<R(Args...)>::call()'s own `this->invoke_(...)` -- so `R`
// here cannot be a reference (spec ch06.2(24): no mechanism for a
// class/closure to carry a named lifetime-group parameter); wrapping
// that same pointer in std::expected does not change this -- std::expected
// stores its T by value, and a pointer is exactly as reference-free
// wrapped as it was bare. Left default-constructed to no_module_resolver
// below for any caller with no imports to resolve -- never actually
// invoked unless the source being parsed contains a real `import`
// declaration, so every existing import-free caller (the whole test
// suite, today) is unaffected.
using ModuleResolver = std::function<std::expected<const Program*, ParseError>(const std::string&)>;

// Callback types for parse_record_body_into's two injection points (a
// class-scope `using` handler and a field-adder), used in place of the
// function's own former `template <typename HandleUsingFn, typename
// AddFieldFn>` parameters. Self-hosting discovered that instantiating
// this (fairly large) function template at each of its two call sites
// -- each with a distinct closure type -- corrupts one clone's own
// `std::expected<void, ParseError>` return-type codegen (the IR
// verifier reports a mismatched, unrelated `std::expected<long,
// ParseError>` shape for the affected clone's `ret` instruction,
// regardless of how the final return statement itself is spelled): a
// real, narrow generic-function-monomorphization codegen bug, not
// anything wrong with this function's own logic. Type-erasing both
// callbacks via std::function (exactly the same real, working
// workaround already used for ModuleResolver above) sidesteps the
// per-call-site monomorphization entirely, at the routine, negligible
// cost of one indirect call per invocation (each callback is called
// at most once per record-body member/using-declaration parsed).
//
// RecordUsingHandlerFn itself returns a plain `bool` rather than
// `std::expected<void, ParseError>` for a second, independent reason:
// the borrow checker can only resolve a "borrow source root" for a
// *direct*, statically-known, reference-returning call (see
// movecheck/borrows.cppm's resolve_borrow_source_root, ExprKind::Call
// case) -- never for an indirect call through a stored, type-erased
// function pointer like std::function's own `invoke_` -- so
// constructing any class-typed local (e.g. `std::expected<void,
// ParseError> x{handle_using(...)}`) directly from this callback's
// result is rejected outright, regardless of how it's spelled. See
// pending_using_error_ below for how the real ParseError travels back
// out on failure instead.
using RecordUsingHandlerFn = std::function<bool(AccessSpecifier)>;
// By-reference parameters here (rather than the more natural-looking
// by-value ones) sidestep a separate, independent std::function gap:
// forwarding a by-value CLASS-typed argument through the generic
// function<R(Args...)>::call -> invoke_ function-pointer chain
// (std_functional.scpp) requires the checker to treat the forwarded
// value as either an implicitly copyable same-type source or a fresh
// std::move'd value, but a *variadic pack* element forwarded as
// `args...` inside that shared, one-size-fits-all template body is
// neither (there's no way to write a per-element std::move over a
// pack) -- so the checker rejects it outright for any class-typed
// pack element (e.g. std::optional<Initializer>), regardless of how
// the call site itself is written. Reference-typed Args avoid the
// by-value-class-argument check entirely (the same reason
// ModuleResolver's own `const std::string&` argument above has never
// hit this), at the cost of the two call sites below copying instead
// of moving into their StructField/ClassField.
//
// The leading SourceLocation is the member declaration's *own* first
// token, handed over explicitly rather than left for each callback to
// recover from the parser's cursor: by the time a field is added its
// whole declaration -- terminating `;` included -- has been consumed,
// so a callback calling current_loc() gets the *next* member's first
// token (or the closing `}`) instead. Passing it makes a field's loc
// come from exactly the same `member_loc` every member function in the
// same record body is already stamped with.
using RecordFieldAdderFn = std::function<void(const SourceLocation&, const Type&, const std::string&, AccessSpecifier,
                                               const std::optional<Initializer>&,
                                               const std::vector<AlignmentSpecifier>&)>;

// Default ModuleResolver: always reports "no module resolvable" via a
// disengaged std::expected. Used (rather than a `{}`-default-constructed,
// empty std::function) so parse_module_declaration's `import` handling
// below never needs to test resolver_ itself for emptiness -- this file
// is compiled both by real clang (genuine std::function, whose only
// empty-test is an implicit/explicit `operator bool()`) and, via the
// self-hosting probe, by scpp itself (this file's own hand-written
// std::function, which has no conversion operators and no nullptr-
// comparison operator at all -- ch06's explicit-cast-for-bool-conversion
// rule) -- there is no single spelling of "is resolver_ empty" valid in
// both worlds, but "did calling it return a disengaged std::expected" (a
// plain `.has_value()` check, exactly like every other ParseError-
// producing call in this file) is trivially valid in both, so that's the
// only check used. This ParseError's own loc is deliberately left
// unknown (default {0, 0}) -- see parse_import_declarations for why that
// is exactly the signal it needs to enrich this fallback message with
// the failing `import` statement's own position.
[[nodiscard]] inline std::expected<const Program*, ParseError> no_module_resolver(const std::string& module_name [[maybe_unused]]) {
    return std::unexpected(ParseError(0, 0, "no module resolver was configured for this build"));
}

// ch11 §11.4: given a same-module partition's fully-qualified key
// ("<module_name>:<partition_name>", e.g. "std:string"), returns a
// *freshly parsed, owned* Program (by value, not a cached reference like
// ModuleResolver above) -- called while parsing an `import :part;` /
// `export import :part;` declaration. A fresh, independently-owned
// Program is required (rather than a shared cached reference) because a
// partition's declarations merge into the importing file *with their
// bodies* (see merge_partition): the partition compiles together with
// whatever imports it, not as a separately-compiled unit, so its
// Function bodies (unique_ptr-owned Stmt trees) must actually be moved
// into the importing Program, not merely referenced. Re-parsing on every
// resolve (rather than caching) sidesteps any "already moved-from"
// concern if more than one sibling file within the same module imports
// the same partition -- a real, if unlikely, v1 limitation: two
// importers of the same partition each get their own independently
// parsed copy (no shared identity), which is fine for merge_partition's
// purposes but would not be the right foundation for anything that ever
// needed cross-partition identity (nothing in v1 does). Returns
// std::expected for exactly the same reason ModuleResolver does above --
// see that alias's own comment for the loc.is_known() convention this
// callback's error must follow. Left default-constructed to
// no_partition_resolver below for any caller with no partitions to
// resolve.
using PartitionResolver = std::function<std::expected<Program, ParseError>(const std::string&)>;

// Default PartitionResolver, mirroring no_module_resolver above: reports
// "no partition resolvable" via a disengaged std::expected, so
// parse_module_declaration's `import :part;` handling below never needs
// to test partition_resolver_ itself for emptiness (same reasoning as
// no_module_resolver). loc is left unknown for the same reason too.
[[nodiscard]] inline std::expected<Program, ParseError> no_partition_resolver(const std::string& partition_key [[maybe_unused]]) {
    return std::unexpected(ParseError(0, 0, "no partition resolver was configured for this build"));
}

[[nodiscard]] std::size_t next_parser_instance_id() {
    static std::size_t counter = 0;
    return ++counter;
}

[[nodiscard]] std::string_view builtin_scalar_keyword_type_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::KwInt: return "int";
        case TokenKind::KwBool: return "bool";
        case TokenKind::KwChar: return "char";
        case TokenKind::KwLong: return "long";
        case TokenKind::KwFloat: return "float";
        case TokenKind::KwDouble: return "double";
        case TokenKind::KwSizeT: return "size_t";
        case TokenKind::KwPtrdiffT: return "ptrdiff_t";
        case TokenKind::KwInt8T: return "int8_t";
        case TokenKind::KwUInt8T: return "uint8_t";
        case TokenKind::KwInt16T: return "int16_t";
        case TokenKind::KwUInt16T: return "uint16_t";
        case TokenKind::KwInt32T: return "int32_t";
        case TokenKind::KwUInt32T: return "uint32_t";
        case TokenKind::KwInt64T: return "int64_t";
        case TokenKind::KwUInt64T: return "uint64_t";
        case TokenKind::KwVoid: return "void";
        default: return {};
    }
}

[[nodiscard]] bool is_type_start_keyword(TokenKind kind) {
    return kind == TokenKind::KwConst || kind == TokenKind::KwUnsigned || kind == TokenKind::KwNullptrT ||
           !builtin_scalar_keyword_type_name(kind).empty();
}

[[nodiscard]] bool is_cast_type_start_keyword(TokenKind kind) {
    return kind == TokenKind::KwUnsigned || kind == TokenKind::KwNullptrT ||
           !builtin_scalar_keyword_type_name(kind).empty();
}

// ch05 §5.14: one injected generic type name's own template-parameter
// shape, as seen from inside its own generic class/struct body (see
// Parser::injected_generic_type_name_stack_'s own comment for why this
// exists). Hoisted to file scope (rather than nested inside Parser,
// where it once lived) because scpp's own class-body grammar doesn't
// yet parse a nested type declaration as a member -- exactly the same
// self-hosting adaptation already applied throughout this file for
// other C++-only conveniences (structured bindings, bare brace-init
// expressions, etc.), not a behavior change. Declared `class` (not
// `struct`, ch04 §4.2/spec ch04): its `std::string`/`std::vector<...>`
// fields are class-typed, and a plain `struct` may only hold scalars,
// pointers, trivial structs/unions, and fixed-size arrays of trivial
// types. scpp also has no positional brace-aggregate-init (`Type{a, b,
// c}` does not fill fields the way it would in real C++), so the one
// call site that builds this with 3 positional arguments needs an
// explicit constructor below.
class InjectedGenericTypeName {
  public:
    virtual ~InjectedGenericTypeName() = default;
    InjectedGenericTypeName() = default;
    InjectedGenericTypeName(const InjectedGenericTypeName&) = default;
    InjectedGenericTypeName& operator=(const InjectedGenericTypeName&) = default;
    InjectedGenericTypeName(InjectedGenericTypeName&&) = default;
    InjectedGenericTypeName& operator=(InjectedGenericTypeName&&) = default;

    InjectedGenericTypeName(std::string spelled, std::string qualified, std::vector<GenericTypeParam> params)
        : spelled_name{std::move(spelled)}, qualified_name{std::move(qualified)}, template_params{std::move(params)} {
        return;
    }

    std::string spelled_name;
    std::string qualified_name;
    std::vector<GenericTypeParam> template_params;
};

// Hoisted to file scope for the same reason as InjectedGenericTypeName
// just above -- originally a type nested inside Parser, moved out
// purely so this file's own source is expressible in scpp's current
// (pre-nested-type-declaration) class-body grammar; see
// Parser::record_tag_kinds_'s own comment for what this is for.
enum class RecordTagKind { Struct, Class, Union };

// ch00 §2/ch01 §1.3: a parsed `[[ ... ]]` attribute-specifier-seq's
// own recognized `scpp::`-namespaced attribute-tokens -- e.g.
// parsing `[[scpp::unsafe]]` yields `{"unsafe"}`. Every attribute
// *not* in the `scpp` namespace (a real C++ standard one like
// `[[nodiscard]]`, or one this parser doesn't yet recognize even
// within `scpp::`, e.g. `scpp::lifetime` -- designed, ch05 §5.3, but
// not yet implemented, tracked for a later milestone) is silently
// parsed and discarded here, exactly like a real C++ compiler
// silently accepts and ignores an attribute it doesn't itself
// define (ch00 §2's own erasure principle, applied to scpp's own
// parser too, not just to a real downstream C++ compiler). Hoisted to
// file scope for the same self-hosting reason as InjectedGenericTypeName/
// RecordTagKind above -- was originally nested inside Parser. Declared
// `class` (not `struct`) for the same reason as InjectedGenericTypeName
// above -- its std::unordered_set<std::string>/std::string/
// LifetimeAnnotation fields are all class-typed. Move-only (no copy
// ctor/assignment declared): its 2 ExprPtr (std::unique_ptr<Expr>)
// fields are themselves move-only, and every call site already only
// ever moves a ParsedAttributes value, never copies one.
class ParsedAttributes {
  public:
    virtual ~ParsedAttributes() = default;
    ParsedAttributes() = default;
    ParsedAttributes(ParsedAttributes&&) = default;
    ParsedAttributes& operator=(ParsedAttributes&&) = default;

    std::unordered_set<std::string> scpp_tokens;
    ExprPtr thread_movable_if_movable_expr;
    ExprPtr thread_movable_if_shareable_expr;
    bool has_nodiscard = false;
    bool has_fallthrough = false;
    std::string nodiscard_reason;
    LifetimeAnnotation lifetime;
    [[nodiscard]] bool has(const std::string& token) const { return scpp_tokens.contains(token); }
};

// The 3 types below (ParsedOutOfLineMemberOwner, OutOfLineMemberKind,
// ParsedOutOfLineMemberDefinition) describe one out-of-line member
// definition (`ClassName::method(...) { ... }`) as it's parsed, before
// it's merged back into its declared member's own Function record --
// hoisted to file scope for the same self-hosting reason as every
// other type just above (was originally nested inside Parser). Declared
// `class` (not `struct`) for the same reason as InjectedGenericTypeName/
// ParsedAttributes above -- its std::string fields are class-typed.
class ParsedOutOfLineMemberOwner {
  public:
    virtual ~ParsedOutOfLineMemberOwner() = default;
    ParsedOutOfLineMemberOwner() = default;
    ParsedOutOfLineMemberOwner(const ParsedOutOfLineMemberOwner&) = default;
    ParsedOutOfLineMemberOwner& operator=(const ParsedOutOfLineMemberOwner&) = default;
    ParsedOutOfLineMemberOwner(ParsedOutOfLineMemberOwner&&) = default;
    ParsedOutOfLineMemberOwner& operator=(ParsedOutOfLineMemberOwner&&) = default;

    std::string spelled_name;
    std::string resolved_name;
    std::string unqualified_name;
};

enum class OutOfLineMemberKind {
    Constructor,
    Destructor,
    Method,
    OperatorDeref,
    OperatorArrow,
    OperatorEqual,
    OperatorNotEqual,
    OperatorAssign,
};

// Declared `class` (not `struct`) for the same reason as the 3 types
// above -- its `Function`/`ParsedOutOfLineMemberOwner`/`std::string`
// fields are all class-typed.
class ParsedOutOfLineMemberDefinition {
  public:
    virtual ~ParsedOutOfLineMemberDefinition() = default;
    ParsedOutOfLineMemberDefinition() = default;
    ParsedOutOfLineMemberDefinition(const ParsedOutOfLineMemberDefinition&) = default;
    ParsedOutOfLineMemberDefinition& operator=(const ParsedOutOfLineMemberDefinition&) = default;
    ParsedOutOfLineMemberDefinition(ParsedOutOfLineMemberDefinition&&) = default;
    ParsedOutOfLineMemberDefinition& operator=(ParsedOutOfLineMemberDefinition&&) = default;

    Function fn;
    ParsedOutOfLineMemberOwner owner;
    OutOfLineMemberKind kind = OutOfLineMemberKind::Method;
    std::string member_name;
    bool is_const_method = false;
};

// Declared `class` (not `struct`, ch04 §4.2/spec ch04) -- like every
// other file-scope type above, Parser has class-typed fields
// (std::vector<Token>, std::string, std::unordered_set<std::string>,
// ModuleResolver/std::function, etc.), which a plain struct cannot
// hold. The pre-existing `public:` label immediately below keeps every
// member's access unchanged; only a mandatory virtual destructor
// (every scpp `class` must declare one, even a trivial one) is new.
class Parser {
public:
    virtual ~Parser() = default;
    explicit Parser(std::vector<Token> tokens, ModuleResolver resolver = no_module_resolver,
                     PartitionResolver partition_resolver = no_partition_resolver, std::string source_path = {})
        : tokens_{std::move(tokens)}, resolver_{std::move(resolver)},
          partition_resolver_{std::move(partition_resolver)}, parser_instance_id_{next_parser_instance_id()} {
        if (!source_path.empty()) source_path_ = std::make_shared<const std::string>(std::move(source_path));
        // ch06 §6: the remaining scalar-family typedef spellings that are
        // intentionally still NOT lexer keywords -- real C++
        // <cstddef>/<stdfloat> names, recognized as pre-registered
        // identifiers from the first line of every program. Spelled out
        // as individual inserts (rather than a range-for over a bare
        // `{...}` list) since this file is also self-hosting-compiled by
        // scpp itself, which does not yet support iterating a raw
        // braced-init-list directly.
        struct_names_.insert("size_t");
        struct_names_.insert("ptrdiff_t");
        struct_names_.insert("float32_t");
        struct_names_.insert("float64_t");
    }

    [[nodiscard]] std::expected<Program, ParseError> parse_program() {
        Program program{};

        current_program_ = &program;
        if (auto _rv = parse_module_declaration(program); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (auto _rv = parse_import_declarations(program); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (auto _rv = parse_top_level_items(program); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        reconcile_identical_extern_c_declarations(program);
        if (auto _rv = reconcile_ordinary_forward_declarations(program); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        qualify_same_namespace_function_calls(program);
        // ch05 §5.11: a reserved, globally-shared witness class for a
        // bare (unconstrained) `auto` parameter -- "the parameter's type
        // is treated as fully opaque... exactly as if it were
        // constrained by a concept whose requires-expression guarantees
        // nothing" (see parse_param_type's own bare-auto handling). `$`
        // can never start (or appear in) a real identifier (see
        // lexer.cppm's is_ident_start/is_ident_continue), so this name
        // can never collide with a real declaration. Registered lazily,
        // only when `bare_auto_used_` was actually set (parse_param_type
        // has no `program` access to push into as it's encountered) --
        // unlike every other class/concept, added *after* parsing
        // everything else so a program never using bare `auto` sees no
        // difference at all in `program.classes`/`program.concepts`'
        // size or contents. No requirement methods at all (unlike a real
        // concept's own witness, parse_concept_def) -- there is nothing
        // to add to Program::functions for it.
        if (bare_auto_used_) {
            ClassDef auto_class{};

            auto_class.name = "$auto";
            auto_class.is_concept_witness = true;
            program.classes.push_back(std::move(auto_class));
            // Paired with the witness class above: an empty-requirements
            // ConceptDef under the same reserved name, so monomorphize_
            // generics' own concept-satisfaction lookup (concepts_by_name_,
            // keyed from Program::concepts) resolves a bare-auto parameter
            // exactly like a real (trivially-satisfied-by-everything)
            // concept -- reusing its existing per-call-site substitution
            // path unchanged, rather than special-casing "$auto"
            // throughout that logic. type_satisfies_concept vacuously
            // returns true for any Named-kind argument type when
            // `requirements` is empty.
            ConceptDef auto_concept{};

            auto_concept.name = "$auto";
            program.concepts.push_back(std::move(auto_concept));
        }
        current_program_ = nullptr;
        return program;
    }

    // Exposes this parser instance's own source_path_ (already threaded
    // into every SourceLocation this parser hands out -- see
    // current_loc()) so the free parse() function below can stamp it onto
    // a ParseError that reaches it with no file identity of its own yet:
    // ParseError's own constructor only ever takes a bare line/column
    // pair (see ParseError above), so every one of its 150+ construction
    // sites (each a `return std::unexpected(ParseError(...))`) stays
    // untouched -- this parse()-boundary stamp is the one place that
    // attaches the file each error actually came from.
    //
    // Returns by const reference rather than by value: `shared_ptr` is
    // deliberately not in movecheck's freely-copyable-value-type
    // allowlist (only side-effect-free "view" wrappers like
    // std::string_view are -- see is_freely_copyable_value_type in
    // movecheck/signatures.cppm), since a shared_ptr copy has an
    // observable refcount side effect, so returning source_path_ by
    // value here would need an explicit fresh copy. The sole call site
    // below assigns straight into a shared_ptr field (a copy-assignment
    // context, which -- unlike returning/passing by value -- accepts a
    // reference source directly), so a reference return avoids the need
    // for that without changing source_path_'s own ownership at all.
    [[nodiscard]] const std::shared_ptr<const std::string>& source_path() const { return source_path_; }

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    ModuleResolver resolver_;
    PartitionResolver partition_resolver_;
    std::shared_ptr<const std::string> source_path_{};
    // Out-of-band failure slot for parse_record_body_into's
    // RecordUsingHandlerFn callback: that callback is a type-erased
    // std::function (see RecordUsingHandlerFn's own comment above) whose
    // call result cannot be bound to a class-typed local at all --
    // scpp's borrow checker can only resolve a "borrow source root" for
    // the result of a *direct*, statically-known, reference-returning
    // call (movecheck/borrows.cppm's resolve_borrow_source_root,
    // ExprKind::Call case), never for an indirect call through a stored
    // function pointer -- so the callback instead returns a plain `bool`
    // (no construction/borrow needed at all, same as ModuleResolver's
    // existing raw-pointer return) and, on failure, stashes the real
    // ParseError here for parse_record_body_into to retrieve right
    // afterward and propagate normally.
    std::optional<ParseError> pending_using_error_{};
    Program* current_program_ = nullptr;
    bool allow_type_lifetime_attributes_ = false;
    // ch11 §11.4: the namespace path currently being parsed into, e.g.
    // inside `namespace std { ... }` this is {"std"}; empty at file
    // scope (today's default -- every existing, non-namespaced file is
    // unaffected). Pushed/popped around a namespace block
    // (parse_namespace_block); qualify_name() joins this onto a bare
    // declared name to produce the fully-qualified form used as that
    // declaration's actual `.name` (see struct_names_ below).
    std::vector<std::string> namespace_stack_{};
    // Names introduced by `struct X { ... };` or `class X { ... };` seen
    // so far. The parser is single-pass, so (like C) either must be
    // declared before it is used as a type; this set is what lets
    // `looks_like_type_start()` recognize `Point p;` as a variable
    // declaration rather than an expression statement starting with the
    // identifier `Point`. Both kinds share one set since, once parsed,
    // they're structurally identical fixed-layout aggregates as far as
    // "is this identifier a type name" is concerned -- `class_names_`
    // below separately tracks *which* of those are specifically classes,
    // for the handful of decisions that do need to tell them apart
    // (constructor-call VarDecl syntax, access control). Entries are
    // fully-qualified (ch11): a struct/class declared inside `namespace
    // std { ... }` is registered as "std::string", not "string" -- see
    // qualify_name.
    std::unordered_set<std::string> struct_names_{};
    // Class names specifically (ch04 §4.2) -- see struct_names_ above for
    // why this is a second, narrower set rather than the only one.
    std::unordered_set<std::string> class_names_{};
    // ch05 §5.11: concept names, keyed the same fully-qualified way as
    // struct_names_/class_names_. Consulted by generic-parameter parsing
    // (`ConceptName auto& name`) to recognize the abbreviated generic-
    // function form: an identifier immediately followed by `auto` is
    // only treated as a concept-constrained parameter when it names an
    // already-declared concept (concepts, like every other declaration
    // this parser handles, must be declared before use).
    std::unordered_set<std::string> concept_names_{};
    // Namespace/module-scope `using Alias = Type;` declarations, keyed by
    // their fully-qualified alias name. The parser resolves aliases eagerly
    // while parsing later type uses, so downstream phases see the aliased
    // underlying Type directly rather than a distinct "alias type" node.
    std::unordered_map<std::string, Type> type_aliases_{};
    std::vector<std::unordered_map<std::string, std::string>> local_type_name_scopes_{};
    std::size_t next_local_type_id_ = 0;
    // Set by parse_record_body_into immediately before it parses a member
    // type definition ([class.mem]: a `struct`/`class`/`enum class`
    // declared inside another type's body), and *consumed* by
    // parse_struct_def/parse_class_def/parse_enum_def at the single point
    // each computes the new type's name. Holds the enclosing type's own
    // already-qualified name, so the member type is named `Outer::Inner`
    // -- the same `A::B` shape a namespace-scope type already has, which
    // is why every later phase (layout, codegen, mangling, module
    // serialization) needs no change to handle one.
    // Consuming it before the nested body is parsed is what makes
    // arbitrary nesting depth work: the inner body's own member loop sets
    // it again, from a `qualified_owner_name` that is by then `A::B`.
    std::string pending_nested_type_owner_{};
    // ch05 §5.11: true once parse_param_type has seen at least one bare
    // (unconstrained) `auto` parameter anywhere in the program --
    // consulted by parse_program at the very end to decide whether to
    // register the shared "$auto" witness class/concept at all (see its
    // own comment for why this is lazy rather than unconditional).
    bool bare_auto_used_ = false;
    // ch05 §5.14: names of every generic `class`/`struct` *template*
    // declaration seen so far (a subset of struct_names_/class_names_,
    // which already register a generic type's own name unconditionally
    // like any other struct/class) -- consulted by parse_unqualified_type
    // to recognize `Name<Arg>` (a generic-type instantiation) instead of
    // a plain `Name` type reference.
    std::unordered_set<std::string> generic_type_names_{};
    // ch05 §5.14: every variadic primary template's own declared
    // parameter list (`template<typename... Ts> class Tuple;`), keyed
    // by its qualified name -- consulted by parse_variadic_specialization
    // to validate a later specialization's own `<...>` argument list
    // actually matches one of the two fixed patterns (an empty pack, or
    // exactly the primary template's own parameter names in order), and
    // to recognize the pack parameter's own name (needed for a
    // specialization's `: private Tuple<Tail...>` base-clause, see
    // BaseSpecifier::pack_arg_name).
    std::unordered_map<std::string, std::vector<GenericTypeParam>> variadic_primary_template_params_{};
    // ch05 §5.14: every ordinary (non-variadic) generic class/struct
    // *primary template*'s own declared parameter list, keyed by its
    // qualified name -- consulted at an instantiation site
    // (parse_unqualified_type's generic-type-argument loop below) to know
    // how many arguments are expected and, for each position, whether it
    // is a type argument or a non-type one parsed as an expression into
    // Type::non_type_args. Ordinary partial specializations deliberately
    // do NOT overwrite this: use sites still parse against the primary
    // template's surface syntax, with later specialization selection left
    // to movecheck.
    //
    // Mixed ordinary templates interleaving type and non-type parameters
    // would need the Type AST to preserve argument order rather than
    // today's split template_args/non_type_args storage, so ordinary
    // generic classes/structs are currently limited to an all-type or
    // all-non-type parameter list; variadic_primary_template_params_
    // above already handles the separate recursive-inheritance variadic
    // family.
    std::unordered_map<std::string, std::vector<GenericTypeParam>> ordinary_generic_type_template_params_{};
    // ch05 §5.14: InjectedGenericTypeName is now a file-scope type (see
    // its own comment, just above this Parser struct) -- kept for the
    // exact same "name available for lookup while parsing this generic
    // class/struct's own body" purpose as before, only the type
    // definition's location changed.
    std::vector<InjectedGenericTypeName> injected_generic_type_name_stack_{};
    // ch05 §5.14: RecordTagKind is now a file-scope type (see its own
    // comment, just above this Parser struct) -- kept for the exact
    // same "which of struct/class/union tag a name was declared with"
    // purpose as before, only the type definition's location changed.
    std::unordered_map<std::string, RecordTagKind> record_tag_kinds_{};
    // ch05 §5.11: every full-header-form generic function's own declared
    // template parameter list (`template<size_t I, typename Head,
    // typename... Tail> Head& get(...)`), keyed by its qualified name --
    // consulted at a *call* site (parse_postfix's Identifier-then-`(`
    // handling) to recognize `name<Args>(...)` as an explicit-template-
    // argument call (rather than misparsing `<`/`>` as comparison
    // operators, the classic ambiguity) and to know, for each argument
    // position, whether to parse a type or a non-type expression.
    std::unordered_map<std::string, std::vector<GenericTypeParam>> generic_function_template_params_{};
    // Non-empty only while parsing one full-header-form generic function's
    // signature/body (`template<...> ReturnType name(...) { ... }`). Lets the
    // ordinary parameter parser recognize `Args... args` as a real template
    // parameter pack rather than rejecting every non-concept pack as the
    // abbreviated-generic-only form.
    std::vector<GenericTypeParam> current_function_template_params_{};
    // Non-empty only while parsing the body/signature surface of one
    // generic class/specialization. Lets member parameter parsing and
    // function-pointer declarators recognize named pack parameters from
    // the enclosing type template as real pack expansions.
    std::vector<GenericTypeParam> current_class_template_params_{};
    std::size_t generic_template_owner_counter_ = 0;
    std::size_t parser_instance_id_ = 0;
    std::size_t synthesized_for_temp_counter_ = 0;
    int loop_depth_ = 0;
    int switch_depth_ = 0;
    // How deep the recursive descent currently is, counted across the
    // statement and expression cycles (parse_statement -> parse_block ->
    // parse_statement, parse_primary -> parse_expr -> parse_primary, and
    // parse_unary -> parse_unary). Bounded by kMaxNestingDepth so that a
    // deeply nested source file is diagnosed rather than overflowing the
    // host stack; see the derivation on kMaxNestingDepth in scpp.ast.
    int nesting_depth_ = 0;

    [[nodiscard]] const Token& peek() const { return tokens_[pos_]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }

    // The position of the *next* token to be consumed -- called at the
    // start of parsing a new Expr/Stmt/Function, before any of its own
    // tokens are consumed, so the resulting node's `.loc` points at
    // wherever it syntactically begins (see SourceLocation, ast.cppm).
    [[nodiscard]] SourceLocation current_loc() const { return SourceLocation{peek().line, peek().column, source_path_}; }

    const Token& advance() {
        // Deliberately capture the *current* position into a plain scalar
        // local, do the (unrelated) `pos_` write, and only then index into
        // `tokens_` for the return value -- taking a `const Token&` into
        // `tokens_` before writing `pos_` gets rejected by the borrow
        // checker as "cannot write through 'this' while a nested reborrow
        // derived from it is still live", since scpp v0.1's checker treats
        // all borrows reachable through `this` as one whole-object unit
        // rather than tracking `tokens_` and `pos_` as disjoint fields.
        std::size_t current = pos_;
        if (pos_ + 1 < tokens_.size()) pos_++;
        return tokens_[current];
    }

    bool match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    // Returns a *copy* of the matched token (rather than the `const Token&`
    // this returned before conversion) since std::expected<T, E> cannot hold
    // a reference type T -- neither real C++23's std::expected, nor scpp's
    // own future one (libs/std/expected/std_expected.scpp stores T by value
    // in an aligned byte buffer, which is likewise incompatible with
    // reference types). Token (lexer.cppm) is a small, cheaply-copyable
    // value (a TokenKind, a std::string_view, and two ints), so this is not
    // a meaningful cost.
    [[nodiscard]] std::expected<Token, ParseError> expect(TokenKind kind, const std::string& what) {
        if (!check(kind)) {
            const Token& tok = peek();
            {
                std::string _msg_457{"expected "};
                _msg_457 += what;
                _msg_457 += " but found '";
                _msg_457 += std::string(tok.text.data(), tok.text.size());
                _msg_457 += "'";
                return std::unexpected(ParseError(tok.line, tok.column, _msg_457));
            }
        }
        return advance();
    }

    [[nodiscard]] bool looks_like_type_start() const {
        const Token& tok = peek();
        if (tok.kind == TokenKind::KwAlignas) return true;
        if (tok.kind == TokenKind::KwConstexpr) return true;
        if (is_type_start_keyword(tok.kind)) return true;
        // ch05 §5.12: a bare `auto` at statement start unambiguously
        // means an auto-typed var-decl (`auto f = expr;`) -- the only
        // way to name a closure's own compiler-synthesized, otherwise
        // unspellable type. `auto`'s *other* legal appearance (`Concept
        // auto` in parameter position, parse_param_type) is never the
        // very first token of a statement, so there's no ambiguity here.
        if (tok.kind == TokenKind::KwAuto) return true;
        if (peek_std_qualified_builtin_scalar_type_name().has_value()) return true;
        if (check_std_qualified("span")) return true;
        if (tok.kind != TokenKind::Identifier) return false;
        // ch11: a bare identifier might be the *first segment* of a
        // qualified name (`std::string`) rather than a plain type name --
        // peek_qualified_name looks ahead through the whole `::` chain so
        // the fully-qualified form is what gets checked against
        // struct_names_ (which registers declarations under exactly that
        // form -- see parse_class_def/parse_struct_def).
        return is_visible_type_name(peek_qualified_name());
    }

    // ch06 §6: a simplified, offset-based variant of looks_like_type_
    // start above -- no std::-qualified-name lookahead (a cast's own
    // target type is never realistically std::unique_ptr<T>/
    // std::span<T> in this version) -- used only by parse_unary's own
    // C-style-cast lookahead, `(T)expr`, which needs to peek past the
    // `(` at `offset` positions ahead without disturbing `pos_` at all
    // (unlike looks_like_type_start, always checked at the current
    // position).
    [[nodiscard]] bool looks_like_type_start_at(std::size_t offset) const {
        const Token& tok = peek_at(offset);
        if (tok.kind == TokenKind::KwAlignas) return true;
        if (is_cast_type_start_keyword(tok.kind)) return true;
        if (peek_std_qualified_builtin_scalar_type_name(offset).has_value()) return true;
        if (tok.kind == TokenKind::Identifier && tok.text == "std" && peek_at(offset + 1).kind == TokenKind::ColonColon &&
            peek_at(offset + 2).kind == TokenKind::Identifier && peek_at(offset + 2).text == "span") {
            return true;
        }
        return tok.kind == TokenKind::Identifier && is_visible_type_name(std::string(tok.text.data(), tok.text.size()));
    }

    // Bounds-safe lookahead: returns the token `offset` positions ahead of
    // the current one, or the (always-last) EndOfFile token if that would
    // run past the end of the stream.
    [[nodiscard]] const Token& peek_at(std::size_t offset) const {
        std::size_t idx = pos_ + offset;
        return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
    }

    [[nodiscard]] std::optional<std::string> referenced_pack_type_param_name(const Type& type) const {
        // Unwraps one level of TypeKind::Reference by recursing directly on
        // the pointee, instead of rebinding a local `const Type&` to a
        // ternary (scpp's borrow checker only allows a reference to borrow a
        // plain local/field/array-element/raw-pointer-deref/call-result, not
        // an arbitrary ternary expression) or reassigning a raw `Type*`
        // (which would require an `[[scpp::unsafe]]` block to dereference).
        // A Reference type's own name/template_args/pointee/etc. are never
        // meaningful, so recursing here is equivalent to the original
        // "unwrap once, then keep checking" behavior for every real input.
        if (type.kind == TypeKind::Reference && type.pointee != nullptr) {
            return referenced_pack_type_param_name(*type.pointee);
        }
        if (type.kind == TypeKind::Named) {
            for (const GenericTypeParam& param : current_class_template_params_) {
                if (!param.is_pack || param.is_non_type) continue;
                if (param.name == type.name) return param.name;
            }
            for (const GenericTypeParam& param : current_function_template_params_) {
                if (!param.is_pack || param.is_non_type) continue;
                if (param.name == type.name) return param.name;
            }
        }
        for (const Type& arg : type.template_args) {
            if (std::optional<std::string> found = referenced_pack_type_param_name(arg); found.has_value()) return found;
        }
        if (type.pointee != nullptr) {
            if (std::optional<std::string> found = referenced_pack_type_param_name(*type.pointee); found.has_value()) return found;
        }
        if (type.element != nullptr) {
            if (std::optional<std::string> found = referenced_pack_type_param_name(*type.element); found.has_value()) return found;
        }
        if (type.function_return != nullptr) {
            if (std::optional<std::string> found = referenced_pack_type_param_name(*type.function_return); found.has_value()) return found;
        }
        for (const Type& param_type : type.function_params) {
            if (std::optional<std::string> found = referenced_pack_type_param_name(param_type); found.has_value()) return found;
        }
        return std::optional<std::string>{};
    }

    // ch05 §5.14: given the offset of a `<` (e.g. a `template<...>`
    // header, or a `Name<...>` specialization/instantiation), returns
    // the offset of the token immediately *after* its own matching `>`
    // -- without consuming anything. Tracks nesting depth (never
    // actually reached in this version -- neither a template header nor
    // a specialization's own argument list ever contains a nested
    // `<...>` -- but doing so is free and more robust than assuming
    // flatness). Used purely for lookahead/dispatch; the real parse
    // that follows re-walks the same tokens structurally.
    [[nodiscard]] std::size_t offset_after_matching_angle(std::size_t less_than_offset) const {
        std::size_t offset = less_than_offset + 1;
        int depth = 1;
        while (depth > 0 && peek_at(offset).kind != TokenKind::EndOfFile) {
            if (peek_at(offset).kind == TokenKind::Less) depth++;
            else if (peek_at(offset).kind == TokenKind::Greater) depth--;
            offset++;
        }
        return offset;
    }

    // Sibling lookahead helper to offset_after_matching_angle: given
    // the offset of the token that would immediately follow `class`/
    // `struct` (i.e. either the type's own name, or the start of an
    // optional class-head attribute-specifier-sequence like
    // `[[nodiscard("...")]]`), returns the offset of the type's own
    // name token -- skipping zero or more `[[...]]` groups without
    // consuming anything. Needed because a class-head attribute is
    // legal directly after `class`/`struct` and before the name (this
    // stdlib already writes `class [[nodiscard(...)]] expected { ... };`
    // for its primary template), so the KwTemplate dispatch below must
    // not assume the name always sits immediately after `class`/
    // `struct` when it peeks ahead to tell a forward declaration/
    // partial specialization apart from an ordinary primary template.
    [[nodiscard]] std::size_t offset_after_attribute_specifier_seq(std::size_t start_offset) const {
        std::size_t offset = start_offset;
        while (peek_at(offset).kind == TokenKind::LBracket && peek_at(offset + 1).kind == TokenKind::LBracket) {
            offset += 2;
            int depth = 2; // two opening brackets ('[[') already consumed above
            while (depth > 0 && peek_at(offset).kind != TokenKind::EndOfFile) {
                if (peek_at(offset).kind == TokenKind::LBracket) depth++;
                else if (peek_at(offset).kind == TokenKind::RBracket) depth--;
                offset++;
            }
        }
        return offset;
    }

    // ParsedAttributes is now a file-scope type (see its own comment
    // above this Parser struct) -- hoisted purely for self-hosting
    // reasons, no behavior change.

    [[nodiscard]] std::expected<std::vector<AlignmentSpecifier>, ParseError> parse_alignment_specifier_seq() {
        std::vector<AlignmentSpecifier> specs{};

        while (check(TokenKind::KwAlignas)) {
            SourceLocation loc = current_loc();
            advance(); // alignas
            auto lparen_result = expect(TokenKind::LParen, "'(' after 'alignas'");
            if (!lparen_result.has_value()) return std::unexpected(std::move(lparen_result).error());
            AlignmentSpecifier spec{};

            spec.loc = loc;
            std::size_t saved_pos = pos_;
            // Speculative parse: attempt the type-operand form first: if
            // parse_type() (or the ')' that must follow it) doesn't pan
            // out, this falls back to the expression-operand form below,
            // exactly like the try/catch(ParseError&) this replaces. The
            // `continue` lives directly inside the same block as the
            // `spec` move (rather than behind a deferred `parsed_as_type`
            // flag checked afterwards) so every path reaching the
            // expression-operand code below provably never moved `spec` --
            // movecheck's CFG-join dataflow can't correlate an arbitrary
            // bool's value with a different variable's move-state across
            // the nested-if merge blocks, so deferring the `continue`
            // behind such a flag reads as "inconsistent init state" there.
            if (looks_like_type_start()) {
                auto type_result = parse_type();
                if (type_result.has_value()) {
                    auto rparen_result = expect(TokenKind::RParen, "')' after alignas type operand");
                    if (rparen_result.has_value()) {
                        spec.operand_is_type = true;
                        spec.type = std::move(type_result).value();
                        specs.push_back(std::move(spec));
                        continue;
                    }
                }
            }
            pos_ = saved_pos;
            auto expr_result = parse_expr();
            if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
            spec.expr = std::move(expr_result).value();
            auto rparen_result = expect(TokenKind::RParen, "')' after alignas expression operand");
            if (!rparen_result.has_value()) return std::unexpected(std::move(rparen_result).error());
            specs.push_back(std::move(spec));
        }
        return specs;
    }

    ClassDef clone_class_def(const ClassDef& def) {
        ClassDef clone{};

        clone.name = def.name;
        clone.fields = def.fields;
        clone.namespace_path = def.namespace_path;
        clone.is_exported = def.is_exported;
        clone.is_compile_time_dependency = def.is_compile_time_dependency;
        clone.owning_module = def.owning_module;
        clone.is_concept_witness = def.is_concept_witness;
        clone.template_params = def.template_params;
        clone.template_owner_id = def.template_owner_id;
        clone.is_forward_declaration = def.is_forward_declaration;
        clone.is_synthetic_check_only = def.is_synthetic_check_only;
        clone.is_interface = def.is_interface;
        clone.base_specifiers.clear();
        for (const BaseSpecifier& base : def.base_specifiers) {
            BaseSpecifier cloned = base;
            cloned.base_type = Type{base.base_type};
            clone.base_specifiers.push_back(std::move(cloned));
        }
        clone.using_declarations = def.using_declarations;
        clone.is_variadic_primary_template = def.is_variadic_primary_template;
        clone.is_variadic_specialization = def.is_variadic_specialization;
        clone.is_partial_specialization = def.is_partial_specialization;
        clone.specialization_template_args = def.specialization_template_args;
        clone.thread_movable_override = def.thread_movable_override;
        clone.thread_shareable_override = def.thread_shareable_override;
        clone.is_nodiscard = def.is_nodiscard;
        clone.nodiscard_reason = def.nodiscard_reason;
        if (def.thread_movable_if_movable_expr != nullptr) {
            clone.thread_movable_if_movable_expr = deep_clone_expr(*def.thread_movable_if_movable_expr);
        }
        if (def.thread_movable_if_shareable_expr != nullptr) {
            clone.thread_movable_if_shareable_expr = deep_clone_expr(*def.thread_movable_if_shareable_expr);
        }
        return clone;
    }

    // ch00 §2: parses zero or more leading `[[ attr-list ]]` attribute-
    // specifier-seqs -- real C++ grammar already gives a compound-
    // statement, a function declaration, a class-head, and a parameter-
    // declaration each an optional leading (or, for a parameter, a
    // trailing) attribute-specifier-seq (the same slot `[[likely]]`/
    // `[[noreturn]]`/`[[deprecated]]` already use); this parser
    // recognizes `[[`/`]]` as two consecutive `[`/`]` tokens rather than
    // a dedicated combined lexer token, since nothing in lexer.cppm ever
    // needed one before this (no existing scpp construct starts with a
    // literal `[[`). Each bracketed group holds a comma-separated list
    // of attributes, each spelled `token` or `namespace::token`, with an
    // optional single-identifier argument (e.g. `scpp::lifetime(name)`)
    // parsed and discarded -- this parser doesn't act on any argument
    // yet. Returns every recognized `scpp`-namespaced token found across
    // every group; a bare (non-namespaced) attribute, or one in any
    // other namespace, is always silently ignored (scpp defines nothing
    // outside its own `scpp` namespace).
    [[nodiscard]] std::expected<void, ParseError> skip_attribute_arguments() {
        int depth = 1;
        while (depth > 0) {
            if (check(TokenKind::EndOfFile)) {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column, "unterminated attribute argument list"));
            }
            if (match(TokenKind::LParen)) {
                depth++;
            } else if (match(TokenKind::RParen)) {
                depth--;
            } else {
                advance();
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> merge_lifetime_attribute(LifetimeAnnotation& dst, const LifetimeAnnotation& src, const Token& tok,
                                  const char* where) {
        if (!src.present()) return {};
        if (dst.present()) {
            {
                std::string _msg_783{where};
                _msg_783 += " may bear at most one '[[scpp::lifetime(name)]]' attribute";
                return std::unexpected(ParseError(tok.line, tok.column,
                             _msg_783));
            }
        }
        dst = src;
        return {};
    }

    void collect_type_lifetime_annotations(const Type& type, int& out_count, LifetimeAnnotation& out_first) const {
        if (type.lifetime.present()) {
            if (out_count == 0) out_first = type.lifetime;
            out_count++;
        }
        if (type.pointee != nullptr) collect_type_lifetime_annotations(*type.pointee, out_count, out_first);
        if (type.element != nullptr) collect_type_lifetime_annotations(*type.element, out_count, out_first);
        if (type.function_return != nullptr) collect_type_lifetime_annotations(*type.function_return, out_count, out_first);
        for (const Type& param : type.function_params) collect_type_lifetime_annotations(param, out_count, out_first);
        for (const Type& arg : type.template_args) collect_type_lifetime_annotations(arg, out_count, out_first);
    }

    void clear_type_lifetime_annotations(Type& type) {
        LifetimeAnnotation cleared_lifetime{};
        type.lifetime = cleared_lifetime;
        if (type.pointee != nullptr) clear_type_lifetime_annotations(*type.pointee);
        if (type.element != nullptr) clear_type_lifetime_annotations(*type.element);
        if (type.function_return != nullptr) clear_type_lifetime_annotations(*type.function_return);
        for (Type& param : type.function_params) clear_type_lifetime_annotations(param);
        for (Type& arg : type.template_args) clear_type_lifetime_annotations(arg);
    }

    [[nodiscard]] std::expected<void, ParseError> hoist_type_lifetime_annotation(Type& type, LifetimeAnnotation& dst, const Token& tok, const char* where) {
        int annotation_count = 0;
        LifetimeAnnotation first_annotation{};

        collect_type_lifetime_annotations(type, annotation_count, first_annotation);
        if (annotation_count > 1) {
            {
                std::string _msg_814{where};
                _msg_814 += " may bear at most one '[[scpp::lifetime(name)]]' attribute";
                return std::unexpected(ParseError(tok.line, tok.column,
                             _msg_814));
            }
        }
        if (annotation_count > 0) {
            auto merge_result = merge_lifetime_attribute(dst, first_annotation, tok, where);
            if (!merge_result.has_value()) return std::unexpected(std::move(merge_result).error());
        }
        clear_type_lifetime_annotations(type);
        return {};
    }

    // These two helpers replace what was originally a single generic
    // pass-through wrapper (`template<typename Fn> auto with_type_lifetime_
    // attributes_enabled(Fn&& fn)`, calling a caller-supplied lambda) --
    // the self-hosting parser doesn't yet support a generic function
    // parameterized on a callable type deduced from a forwarding-reference
    // parameter together with `auto`/`decltype(auto)` return-type
    // deduction, so each of the two distinct call shapes actually used
    // (parse_type() and parse_param_type(...)) gets its own concrete,
    // non-generic wrapper instead. Since fn() now signals failure via a
    // returned std::expected<T, ParseError> rather than by throwing (this
    // file's own conversion, ParseError's own comment), there is no longer
    // anything for a try/catch here to guard against -- `allow_type_
    // lifetime_attributes_` only ever needs restoring on the wrapped
    // call's ordinary return, whether that return holds a value or a
    // ParseError.
    [[nodiscard]] std::expected<Type, ParseError> parse_type_with_lifetime_attributes_enabled() {
        bool saved = allow_type_lifetime_attributes_;
        allow_type_lifetime_attributes_ = true;
        auto result = parse_type();
        allow_type_lifetime_attributes_ = saved;
        return result;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_param_type_with_lifetime_attributes_enabled(std::string& out_generic_concept) {
        bool saved = allow_type_lifetime_attributes_;
        allow_type_lifetime_attributes_ = true;
        auto result = parse_param_type(out_generic_concept);
        allow_type_lifetime_attributes_ = saved;
        return result;
    }

    [[nodiscard]] std::expected<ParsedAttributes, ParseError> parse_attribute_specifier_seq() {
        ParsedAttributes result{};

        while (check(TokenKind::LBracket) && peek_at(1).kind == TokenKind::LBracket) {
            advance(); // '['
            advance(); // '['
            if (!check(TokenKind::RBracket)) {
                // Rewritten from `do { ... } while (match(TokenKind::
                // Comma));` -- the self-hosting parser has no `do`
                // keyword/AST representation at all (confirmed absent
                // from the whole compiler source tree, not just this
                // file), so every do-while in this file is rewritten as
                // an equivalent `while (true) { ...; if (!cond) break; }`,
                // which still runs the body at least once and evaluates
                // the loop condition at exactly the same point (right
                // after the body) that the original do-while's implicit
                // condition check would have.
                while (true) {
                    std::string ns{};

                    auto token_result = expect(TokenKind::Identifier, "attribute token");
                    if (!token_result.has_value()) return std::unexpected(std::move(token_result).error());
                    std::string token = std::string(token_result.value().text.data(), token_result.value().text.size());
                    if (match(TokenKind::ColonColon)) {
                        ns = token;
                        auto token2_result = expect(TokenKind::Identifier, "attribute token");
                        if (!token2_result.has_value()) return std::unexpected(std::move(token2_result).error());
                        token = std::string(token2_result.value().text.data(), token2_result.value().text.size());
                    }
                    if (match(TokenKind::LParen)) {
                        if (ns.empty() && token == "nodiscard") {
                            if (!check(TokenKind::StringLiteral)) {
                                const Token& tok = peek();
                                return std::unexpected(ParseError(tok.line, tok.column,
                                                 "'[[nodiscard]]' only accepts an optional single string literal reason"));
                            }
                            result.has_nodiscard = true;
                            auto string_tok_result = expect(TokenKind::StringLiteral, "a string literal");
                            if (!string_tok_result.has_value()) return std::unexpected(std::move(string_tok_result).error());
                            auto reason_result = decode_adjacent_string_literals(string_tok_result.value());
                            if (!reason_result.has_value()) return std::unexpected(std::move(reason_result).error());
                            result.nodiscard_reason = std::move(reason_result).value();
                            auto rparen_result = expect(TokenKind::RParen, "')'");
                            if (!rparen_result.has_value()) return std::unexpected(std::move(rparen_result).error());
                        } else if (ns == "scpp" && token == "thread_movable_if") {
                            auto movable_expr_result = parse_expr();
                            if (!movable_expr_result.has_value()) return std::unexpected(std::move(movable_expr_result).error());
                            result.thread_movable_if_movable_expr = std::move(movable_expr_result).value();
                            auto comma_result = expect(TokenKind::Comma, "','");
                            if (!comma_result.has_value()) return std::unexpected(std::move(comma_result).error());
                            auto shareable_expr_result = parse_expr();
                            if (!shareable_expr_result.has_value()) return std::unexpected(std::move(shareable_expr_result).error());
                            result.thread_movable_if_shareable_expr = std::move(shareable_expr_result).value();
                            auto rparen_result = expect(TokenKind::RParen, "')'");
                            if (!rparen_result.has_value()) return std::unexpected(std::move(rparen_result).error());
                        } else if (ns == "scpp" && token == "lifetime") {
                            if (!check(TokenKind::Identifier) && !check(TokenKind::KwThis)) {
                                const Token& tok = peek();
                                return std::unexpected(ParseError(tok.line, tok.column,
                                                 "'[[scpp::lifetime(name)]]' requires exactly one identifier argument"));
                            }
                            if (check(TokenKind::KwThis)) {
                                auto group_tok_result = expect(TokenKind::KwThis, "lifetime group name");
                                if (!group_tok_result.has_value()) return std::unexpected(std::move(group_tok_result).error());
                                result.lifetime.name = std::string(group_tok_result.value().text.data(), group_tok_result.value().text.size());
                            } else {
                                auto group_tok_result = expect(TokenKind::Identifier, "lifetime group name");
                                if (!group_tok_result.has_value()) return std::unexpected(std::move(group_tok_result).error());
                                result.lifetime.name = std::string(group_tok_result.value().text.data(), group_tok_result.value().text.size());
                            }
                            if (!check(TokenKind::RParen)) {
                                const Token& tok = peek();
                                return std::unexpected(ParseError(tok.line, tok.column,
                                                 "'[[scpp::lifetime(name)]]' requires exactly one identifier argument"));
                            }
                            auto rparen_result = expect(TokenKind::RParen, "')'");
                            if (!rparen_result.has_value()) return std::unexpected(std::move(rparen_result).error());
                        } else {
                            auto skip_result = skip_attribute_arguments();
                            if (!skip_result.has_value()) return std::unexpected(std::move(skip_result).error());
                        }
                    }
                    if (ns.empty() && token == "nodiscard") result.has_nodiscard = true;
                    if (ns.empty() && token == "fallthrough") result.has_fallthrough = true;
                    if (ns == "scpp") result.scpp_tokens.insert(token);
                    if (!(match(TokenKind::Comma))) break;
                }
            }
            auto rbracket1_result = expect(TokenKind::RBracket, "']'");
            if (!rbracket1_result.has_value()) return std::unexpected(std::move(rbracket1_result).error());
            auto rbracket2_result = expect(TokenKind::RBracket, "']'");
            if (!rbracket2_result.has_value()) return std::unexpected(std::move(rbracket2_result).error());
        }
        return std::move(result);
    }

    [[nodiscard]] std::expected<void, ParseError> reject_packed_attribute(const ParsedAttributes& attrs, const Token& attr_start_tok, const char* what) {
        if (!attrs.has("packed")) return {};
        {
            std::string _msg_950{"'[[scpp::packed]]' cannot appertain to "};
            _msg_950 += std::string(what);
            _msg_950 += " -- only to a struct or union declaration (spec §9.2)";
            return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                         _msg_950));
        }
    }

    [[nodiscard]] std::expected<void, ParseError> reject_alignment_specifiers(const std::vector<AlignmentSpecifier>& specs, const char* what) {
        if (specs.empty()) return {};
        const SourceLocation& loc = specs.front().loc;
        {
            std::string _msg_958{"'alignas' cannot appertain to "};
            _msg_958 += std::string(what);
            _msg_958 += " -- only to a variable declaration, a non-static data member declaration, or a ";
            _msg_958 += "struct/class/union declaration (spec §9.3)";
            return std::unexpected(ParseError(loc.line, loc.column,
                         _msg_958));
        }
    }

    [[nodiscard]] std::expected<void, ParseError> reject_lifetime_attribute(const ParsedAttributes& attrs, const Token& attr_start_tok, const char* what) {
        if (!attrs.lifetime.present()) return {};
        {
            std::string _msg_966{"'[[scpp::lifetime(name)]]' cannot appertain to "};
            _msg_966 += std::string(what);
            _msg_966 += " -- only to an eligible parameter declaration or function declarator";
            return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                         _msg_966));
        }
    }


    [[nodiscard]] std::expected<FunctionEvalMode, ParseError> parse_optional_function_eval_mode() {
        FunctionEvalMode mode = FunctionEvalMode::RuntimeOnly;
        if (match(TokenKind::KwConstexpr)) {
            mode = FunctionEvalMode::Constexpr;
        } else if (match(TokenKind::KwConsteval)) {
            mode = FunctionEvalMode::Consteval;
        }
        if (mode != FunctionEvalMode::RuntimeOnly &&
            (check(TokenKind::KwConstexpr) || check(TokenKind::KwConsteval))) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                              "a declaration may specify at most one of 'constexpr' or 'consteval'"));
        }
        return mode;
    }

    [[nodiscard]] BaseClassKind classify_base_name(const Program& program, const std::string& base_name) const {
        for (const ClassDef& def : program.classes) {
            if (def.name == base_name) return def.is_interface ? BaseClassKind::Interface : BaseClassKind::OrdinaryClass;
        }
        return BaseClassKind::Unknown;
    }

    [[nodiscard]] BaseSpecifier make_named_base_specifier(const Program& program, const std::string& base_name,
                                                          AccessSpecifier access) const {
        BaseSpecifier base{};

        base.base_type = named_type(base_name);
        base.access = access;
        base.kind = classify_base_name(program, base_name);
        return base;
    }

    [[nodiscard]] std::expected<BaseSpecifier, ParseError> parse_named_class_base_specifier(const Program& program) {
        AccessSpecifier access = AccessSpecifier::Private;
        bool access_spelled = false;
        bool is_virtual = false;
        bool consumed_specifier = true;
        while (consumed_specifier) {
            consumed_specifier = false;
            if (!access_spelled && match(TokenKind::KwPublic)) {
                access = AccessSpecifier::Public;
                access_spelled = true;
                consumed_specifier = true;
                continue;
            }
            if (!access_spelled && match(TokenKind::KwPrivate)) {
                access = AccessSpecifier::Private;
                access_spelled = true;
                consumed_specifier = true;
                continue;
            }
            if (!is_virtual && match(TokenKind::KwVirtual)) {
                is_virtual = true;
                consumed_specifier = true;
            }
        }
        const Token& base_tok = peek();
        bool explicit_global = check(TokenKind::ColonColon);
        std::string spelled_name{};

        if (explicit_global) {
            auto spelled_result = parse_global_qualified_name();
            if (!spelled_result.has_value()) return std::unexpected(std::move(spelled_result).error());
            spelled_name = std::move(spelled_result).value();
        } else {
            spelled_name = parse_qualified_name();
        }
        std::string base_name{};
        if (explicit_global) {
            base_name = spelled_name;
        } else {
            base_name = resolve_visible_type_name(spelled_name);
        }
        if (base_name.empty() || !class_names_.contains(base_name)) {
            {
                std::string _msg_1042{"'"};
                _msg_1042 += spelled_name;
                _msg_1042 += "' is not a declared class -- a base class must be declared before use ";
                _msg_1042 += "(ch05 §5.14), and only a class (never a struct, ch04 §4.1) may be one";
                return std::unexpected(ParseError(base_tok.line, base_tok.column,
                             _msg_1042));
            }
        }
        BaseSpecifier base = make_named_base_specifier(program, base_name, access);
        base.is_virtual = is_virtual;
        return base;
    }

    [[nodiscard]] std::expected<void, ParseError> parse_named_class_base_clause(const Program& program, std::vector<BaseSpecifier>& bases) {
        if (!match(TokenKind::Colon)) return {};
        while (true) {
            auto base_result = parse_named_class_base_specifier(program);
            if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
            BaseSpecifier __base_result_value = std::move(base_result).value();
            bases.push_back(std::move(__base_result_value));
            if (!(match(TokenKind::Comma))) break;
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_class_using_declaration(ClassDef& def, AccessSpecifier current_access) {
        auto using_tok_result = expect(TokenKind::KwUsing, "'using'");
        if (!using_tok_result.has_value()) return std::unexpected(std::move(using_tok_result).error());
        Token using_tok = using_tok_result.value();
        bool explicit_global = match(TokenKind::ColonColon);
        std::vector<std::string> segments{};

        auto first_seg_result = expect(TokenKind::Identifier, "base class name");
        if (!first_seg_result.has_value()) return std::unexpected(std::move(first_seg_result).error());
        segments.push_back(std::string(first_seg_result.value().text.data(), first_seg_result.value().text.size()));
        while (match(TokenKind::ColonColon)) {
            auto seg_result = expect(TokenKind::Identifier, "identifier after '::'");
            if (!seg_result.has_value()) return std::unexpected(std::move(seg_result).error());
            segments.push_back(std::string(seg_result.value().text.data(), seg_result.value().text.size()));
        }
        if (segments.size() < 2) {
            return std::unexpected(ParseError(using_tok.line, using_tok.column,
                             "a class-scope using declaration must name a base member as 'using Base::member;'"));
        }
        // std::move currently requires a plain identifier argument (no
        // member/subscript/call expressions), so extract via a subscript
        // copy instead of std::move(segments.back()); segments.pop_back()
        // right below discards the original slot anyway.
        std::string member_name = segments[segments.size() - 1];
        segments.pop_back();
        std::string spelled_base_name{};

        for (std::size_t i = 0; i < segments.size(); i++) {
            if (i != 0) spelled_base_name += "::";
            spelled_base_name += segments[i];
        }
        std::string base_name{};
        if (explicit_global) {
            base_name = spelled_base_name;
        } else {
            base_name = resolve_visible_type_name(spelled_base_name);
        }
        if (base_name.empty() || !class_names_.contains(base_name)) {
            {
                std::string _msg_1092{"'"};
                _msg_1092 += spelled_base_name;
                _msg_1092 += "' is not a declared class -- a class-scope using declaration must name a ";
                _msg_1092 += "declared base class";
                return std::unexpected(ParseError(using_tok.line, using_tok.column,
                             _msg_1092));
            }
        }
        auto semi_result = expect(TokenKind::Semicolon, "';'");
        if (!semi_result.has_value()) return std::unexpected(std::move(semi_result).error());
        def.using_declarations.push_back(ClassUsingDeclaration{std::move(base_name), std::move(member_name),
                                                               current_access});
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_type_alias_decl(Program& program, bool is_exported) {
        auto using_tok_result = expect(TokenKind::KwUsing, "'using'");
        if (!using_tok_result.has_value()) return std::unexpected(std::move(using_tok_result).error());
        Token using_tok = using_tok_result.value();
        SourceLocation loc{using_tok.line, using_tok.column, source_path_};
        auto name_result = expect(TokenKind::Identifier, "alias name");
        if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
        std::string bare_name{name_result.value().text.data(), name_result.value().text.size()};
        std::string qualified_name = qualify_name(bare_name);
        auto assign_result = expect(TokenKind::Assign, "'='");
        if (!assign_result.has_value()) return std::unexpected(std::move(assign_result).error());
        auto underlying_type_result = parse_type();
        if (!underlying_type_result.has_value()) return std::unexpected(std::move(underlying_type_result).error());
        Type underlying_type = std::move(underlying_type_result).value();
        auto semi_result = expect(TokenKind::Semicolon, "';'");
        if (!semi_result.has_value()) return std::unexpected(std::move(semi_result).error());
        if (type_aliases_.contains(qualified_name) || struct_names_.contains(qualified_name) || concept_names_.contains(qualified_name)) {
            {
                std::string _msg_1121{"'"};
                _msg_1121 += qualified_name;
                _msg_1121 += "' is already declared as a type or concept";
                return std::unexpected(ParseError(using_tok.line, using_tok.column,
                             _msg_1121));
            }
        }
        type_aliases_.emplace(qualified_name, underlying_type);
        TypeAliasDecl alias{};

        alias.loc = loc;
        alias.underlying_type = std::move(underlying_type);
        alias.name = qualified_name;
        alias.namespace_path = namespace_stack_;
        alias.is_exported = is_exported;
        program.type_aliases.push_back(std::move(alias));
        std::string _msg_1132{"type alias '"};
        _msg_1132 += qualified_name;
        _msg_1132 += "'";
        auto export_ctx_result = check_export_context(program, is_exported, namespace_stack_, loc, _msg_1132);
        if (!export_ctx_result.has_value()) return std::unexpected(std::move(export_ctx_result).error());
        return {};
    }

    [[nodiscard]] ExprPtr make_bool_literal_expr(SourceLocation loc, bool value) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::BoolLiteral;
        expr->loc = loc;
        expr->bool_value = value;
        return expr;
    }

    [[nodiscard]] ExprPtr make_integer_literal_expr(SourceLocation loc, std::int64_t value) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::IntegerLiteral;
        expr->loc = loc;
        expr->int_value = value;
        return expr;
    }

    [[nodiscard]] ExprPtr make_identifier_expr(SourceLocation loc, std::string name) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Identifier;
        expr->loc = loc;
        expr->name = std::move(name);
        return expr;
    }

    [[nodiscard]] ExprPtr make_binary_expr(SourceLocation loc, BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Binary;
        expr->loc = loc;
        expr->binary_op = op;
        expr->lhs = std::move(lhs);
        expr->rhs = std::move(rhs);
        return expr;
    }

    [[nodiscard]] ExprPtr make_member_expr(SourceLocation loc, ExprPtr lhs, std::string name) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Member;
        expr->loc = loc;
        expr->lhs = std::move(lhs);
        expr->name = std::move(name);
        return expr;
    }

    [[nodiscard]] ExprPtr make_subscript_expr(SourceLocation loc, ExprPtr lhs, ExprPtr rhs) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Subscript;
        expr->loc = loc;
        expr->lhs = std::move(lhs);
        expr->rhs = std::move(rhs);
        return expr;
    }

    [[nodiscard]] ExprPtr make_call_expr(SourceLocation loc, std::string name, std::vector<ExprPtr> args) {
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::Call;
        expr->loc = loc;
        expr->name = std::move(name);
        expr->args = std::move(args);
        return expr;
    }

    [[nodiscard]] StmtPtr make_expr_stmt(SourceLocation loc, ExprPtr expr) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->loc = loc;
        stmt->expr = std::move(expr);
        return stmt;
    }

    // Both derive from `scpp.ast`'s scalar type model -- see
    // `scalar_type_info` for why that is the only place the twenty
    // names of ch06 §6 are listed.
    [[nodiscard]] bool is_integral_scalar_type_name(std::string_view name) {
        return scpp::is_integral_scalar_type_name(name);
    }

    [[nodiscard]] bool is_float_scalar_type_name(std::string_view name) {
        return scpp::is_float_scalar_type_name(name);
    }

    [[nodiscard]] ExprPtr make_value_initialized_expr(SourceLocation loc, const Type& type) {
        if (type.kind == TypeKind::Reference && type.pointee != nullptr) {
            return make_value_initialized_expr(loc, *type.pointee);
        }
        if (type.kind == TypeKind::Pointer) {
            auto expr = std::make_unique<Expr>();
            expr->kind = ExprKind::IntegerLiteral;
            expr->loc = loc;
            expr->int_value = 0;
            return expr;
        }
        if (type.kind == TypeKind::Named) {
            if (type.name == "bool") {
                auto expr = std::make_unique<Expr>();
                expr->kind = ExprKind::BoolLiteral;
                expr->loc = loc;
                expr->bool_value = false;
                return expr;
            }
            if (is_integral_scalar_type_name(type.name)) {
                auto expr = std::make_unique<Expr>();
                expr->kind = ExprKind::IntegerLiteral;
                expr->loc = loc;
                expr->int_value = 0;
                return expr;
            }
            if (is_float_scalar_type_name(type.name)) {
                auto expr = std::make_unique<Expr>();
                expr->kind = ExprKind::FloatLiteral;
                expr->loc = loc;
                expr->float_value = 0.0;
                return expr;
            }
        }
        // A class/struct (or other non-scalar named) type: build a real
        // ExprKind::ValueInit node with `.type` stamped in directly (we
        // already know the target type statically here, unlike bare
        // `return {};`, which leaves `.type` for monomorphization to fill
        // in later -- see parse_return). This reuses the exact same
        // codegen path (resolve the type's own zero-arg constructor via
        // find_class_def(expr.type.name), else zero-initialize) rather
        // than the previous, buggy `make_call_expr(type_to_string(type),
        // ...)` fallback, which fabricated a fake call whose callee name
        // was the type's human-readable spelling (e.g.
        // "std::shared_ptr<const std::string>") -- never a registered
        // function name, so it always failed to resolve.
        auto expr = std::make_unique<Expr>();
        expr->kind = ExprKind::ValueInit;
        expr->loc = loc;
        expr->type = type;
        return expr;
    }

    [[nodiscard]] StmtPtr make_continue_stmt(SourceLocation loc) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Continue;
        stmt->loc = loc;
        return stmt;
    }

    [[nodiscard]] StmtPtr make_block_stmt(SourceLocation loc) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Block;
        stmt->loc = loc;
        return stmt;
    }

    [[nodiscard]] StmtPtr make_fallthrough_stmt(SourceLocation loc) {
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Fallthrough;
        stmt->loc = loc;
        return stmt;
    }

    // Builds the "run the epilogue expression, then continue" replacement
    // block for a bare `continue;` (see rewrite_loop_continue_with_epilogue
    // below). Factored into its own function -- rather than declared
    // inline in that switch's `case StmtKind::Continue:` -- so that case
    // can end directly in a bare `return ...;` (as scpp's own switch-case
    // grammar requires every non-empty case's own last statement to
    // literally be `break;`/`return ...;`/`continue;`/`[[fallthrough]];`,
    // not merely end with one nested inside a further compound
    // statement).
    [[nodiscard]] StmtPtr make_continue_with_epilogue_block(SourceLocation loc, const Expr& epilogue) {
        auto block = make_block_stmt(loc);
        block->statements.push_back(make_expr_stmt(epilogue.loc, deep_clone_expr(epilogue)));
        block->statements.push_back(make_continue_stmt(loc));
        return block;
    }

    [[nodiscard]] std::string fresh_for_temp_name(std::string_view stem) {
        {
            std::string _msg_1293{"$for_"};
            _msg_1293 += std::string(stem.data(), stem.size());
            _msg_1293 += "_";
            _msg_1293 += std::to_string(synthesized_for_temp_counter_++);
            return _msg_1293;
        }
    }

    [[nodiscard]] bool is_range_for_decl_start() const {
        return at_auto_declaration_start() || looks_like_type_start();
    }

    [[nodiscard]] StmtPtr rewrite_loop_continue_with_epilogue(StmtPtr stmt, const Expr& epilogue) {
        switch (stmt->kind) {
            case StmtKind::Continue:
                return make_continue_with_epilogue_block(stmt->loc, epilogue);
            case StmtKind::If:
                if (stmt->then_branch != nullptr) {
                    StmtPtr rewritten_then_branch{};
                    {
                        StmtPtr& then_branch_ref = stmt->then_branch;
                        rewritten_then_branch = rewrite_loop_continue_with_epilogue(std::move(then_branch_ref), epilogue);
                    }
                    stmt->then_branch = std::move(rewritten_then_branch);
                }
                if (stmt->else_branch != nullptr) {
                    StmtPtr rewritten_else_branch{};
                    {
                        StmtPtr& else_branch_ref = stmt->else_branch;
                        rewritten_else_branch = rewrite_loop_continue_with_epilogue(std::move(else_branch_ref), epilogue);
                    }
                    stmt->else_branch = std::move(rewritten_else_branch);
                }
                return stmt;
            case StmtKind::Switch:
                for (SwitchCase& switch_case : stmt->switch_cases) {
                    for (StmtPtr& child : switch_case.statements) {
                        child = rewrite_loop_continue_with_epilogue(std::move(child), epilogue);
                    }
                }
                return stmt;
            case StmtKind::Block:
                for (StmtPtr& child : stmt->statements) {
                    child = rewrite_loop_continue_with_epilogue(std::move(child), epilogue);
                }
                return stmt;
            case StmtKind::While:
                return stmt;
            case StmtKind::VarDecl:
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Fallthrough:
            case StmtKind::ExprStmt:
                return stmt;
        }
        return stmt;
    }

    [[nodiscard]] StmtPtr desugar_classic_for(SourceLocation loc, StmtPtr init_stmt, ExprPtr condition, ExprPtr increment,
                                              StmtPtr body) {
        auto outer_block = make_block_stmt(loc);
        if (init_stmt != nullptr) outer_block->statements.push_back(std::move(init_stmt));

        auto while_stmt = std::make_unique<Stmt>();
        while_stmt->kind = StmtKind::While;
        while_stmt->loc = loc;
        while_stmt->condition = std::move(condition);

        auto body_block = make_block_stmt(body->loc);
        if (increment != nullptr) body = rewrite_loop_continue_with_epilogue(std::move(body), *increment);
        body_block->statements.push_back(std::move(body));
        if (increment != nullptr) body_block->statements.push_back(make_expr_stmt(increment->loc, std::move(increment)));
        while_stmt->then_branch = std::move(body_block);

        outer_block->statements.push_back(std::move(while_stmt));
        return outer_block;
    }

    [[nodiscard]] StmtPtr desugar_range_for(SourceLocation loc, StmtPtr loop_var, ExprPtr range_expr, StmtPtr body) {
        std::string range_name = fresh_for_temp_name("range");
        std::string index_name = fresh_for_temp_name("index");

        auto outer_block = make_block_stmt(loc);

        // The synthesized range storage is only ever used to read the
        // range's size and to initialize the loop variable, so the access
        // it needs is exactly the access the loop variable needs: a
        // mutable `T&` loop variable requires a mutable binding, and
        // every other spelling (`const T&`, `const auto&`, or a by-value
        // copy) only ever reads. Recording that here, where the loop
        // variable's declared type is still in hand, keeps monomorphize
        // from having to infer it from the range expression alone --
        // which over-approximated, taking a *mutable* borrow of the
        // container for a read-only loop and so rejecting sound code:
        //
        //     for (const Item& item : items) { total += items.size(); }
        //
        // was "cannot use 'items' while it is mutably borrowed", purely
        // because `items` itself happened to be mutable.
        bool loop_var_needs_mutable_range = loop_var->type.kind == TypeKind::Reference && loop_var->type.is_mutable_ref;

        auto range_decl = std::make_unique<Stmt>();
        range_decl->kind = StmtKind::VarDecl;
        range_decl->loc = loc;
        range_decl->type = named_type("auto");
        range_decl->var_name = range_name;
        range_decl->is_const = !loop_var_needs_mutable_range;
        range_decl->init = std::move(range_expr);
        outer_block->statements.push_back(std::move(range_decl));

        auto index_decl = std::make_unique<Stmt>();
        index_decl->kind = StmtKind::VarDecl;
        index_decl->loc = loc;
        index_decl->type = named_type("int");
        index_decl->var_name = index_name;
        index_decl->init = make_integer_literal_expr(loc, 0);
        outer_block->statements.push_back(std::move(index_decl));

        auto while_stmt = std::make_unique<Stmt>();
        while_stmt->kind = StmtKind::While;
        while_stmt->loc = loc;
        std::vector<ExprPtr> size_args{};

        size_args.push_back(make_identifier_expr(loc, range_name));
        while_stmt->condition =
            make_binary_expr(loc, BinaryOp::Lt, make_identifier_expr(loc, index_name),
                             make_call_expr(loc, "$for_range_size", std::move(size_args)));

        auto increment = make_binary_expr(
            loc, BinaryOp::Assign, make_identifier_expr(loc, index_name),
            make_binary_expr(loc, BinaryOp::Add, make_identifier_expr(loc, index_name), make_integer_literal_expr(loc, 1)));

        loop_var->init =
            make_subscript_expr(loc, make_identifier_expr(loc, range_name), make_identifier_expr(loc, index_name));

        auto body_block = make_block_stmt(body->loc);
        body = rewrite_loop_continue_with_epilogue(std::move(body), *increment);
        body_block->statements.push_back(std::move(loop_var));
        body_block->statements.push_back(std::move(body));
        body_block->statements.push_back(make_expr_stmt(loc, std::move(increment)));
        while_stmt->then_branch = std::move(body_block);

        outer_block->statements.push_back(std::move(while_stmt));
        return outer_block;
    }

    // Checks (without consuming) for the 3-token sequence `std :: <member>`.
    // Only the still-builtin spellings (`std::move`, parser-only
    // `std::span`, and the lexer-keyword scalar aliases routed through
    // peek_std_qualified_builtin_scalar_type_name below) use this helper;
    // ordinary library names now flow through the general qualified-name
    // path.
    [[nodiscard]] bool check_std_qualified(std::string_view member) const {
        return peek().kind == TokenKind::Identifier && peek().text == "std" &&
               peek_at(1).kind == TokenKind::ColonColon && peek_at(2).text == member;
    }

    [[nodiscard]] bool check_scpp_qualified(std::string_view member) const {
        return peek().kind == TokenKind::Identifier && peek().text == "scpp" &&
               peek_at(1).kind == TokenKind::ColonColon && peek_at(2).kind == TokenKind::Identifier &&
               peek_at(2).text == member;
    }

    void consume_std_qualified() {
        advance(); // std
        advance(); // ::
        advance(); // <member>
    }

    [[nodiscard]] std::optional<std::string> peek_std_qualified_builtin_scalar_type_name(std::size_t offset = 0) const {
        if (peek_at(offset).kind != TokenKind::Identifier || peek_at(offset).text != "std" ||
            peek_at(offset + 1).kind != TokenKind::ColonColon) {
            return std::optional<std::string>{};
        }
        // `std::nullptr_t` -- the spelling real C++ exposes for
        // `nullptr`'s type, normalized here to the same bare
        // `nullptr_t` every other phase compares against. Handled
        // separately from the scalar family below because `nullptr_t`
        // is deliberately not a scalar (it must never convert to an
        // integer), so it has no builtin_scalar_keyword_type_name entry.
        if (peek_at(offset + 2).kind == TokenKind::KwNullptrT) return nullptr_type_name();
        // Deliberately not derived from `scpp.ast`'s scalar type model,
        // unlike every other scalar question in this file: which scalars
        // may be written `std::`-qualified is a fact about spelling, not
        // about the type. These are the ones real C++ declares in
        // <cstdint>/<cstddef>; `int`, `char`, `bool`, `unsigned long`
        // and the rest are keywords, and no C++ program can write
        // `std::int`. The model answers what a type *is*; the lexer
        // keyword table just above and this list answer how it may be
        // written.
        std::string_view name = builtin_scalar_keyword_type_name(peek_at(offset + 2).kind);
        if (name == "size_t" || name == "ptrdiff_t" || name == "int8_t" || name == "uint8_t" ||
            name == "int16_t" || name == "uint16_t" || name == "int32_t" || name == "uint32_t" ||
            name == "int64_t" || name == "uint64_t") {
            return std::string(name.data(), name.size());
        }
        return std::optional<std::string>{};
    }

    // Looks ahead (without consuming anything) at a possibly-qualified
    // name starting at the current token -- `Identifier (:: Identifier)*`
    // -- and returns it joined with "::" (e.g. `std::string`). Returns an
    // empty string if the current token isn't even an Identifier. Used
    // by looks_like_type_start to recognize a namespace-qualified type
    // name (ch11 §11.4/§11.5): struct_names_/class_names_ register
    // declarations under their fully-qualified name (see parse_class_def/
    // parse_struct_def's namespace handling), so checking membership
    // requires the *whole* qualified chain, not just its first segment.
    [[nodiscard]] std::string peek_qualified_name() const {
        if (peek().kind != TokenKind::Identifier) return {};
        std::string joined{peek().text.data(), peek().text.size()};
        std::size_t offset = 1;
        while (peek_at(offset).kind == TokenKind::ColonColon && peek_at(offset + 1).kind == TokenKind::Identifier) {
            joined += "::";
            joined += std::string(peek_at(offset + 1).text.data(), peek_at(offset + 1).text.size());
            offset += 2;
        }
        return joined;
    }

    [[nodiscard]] std::string peek_global_qualified_name() const {
        if (peek().kind != TokenKind::ColonColon || peek_at(1).kind != TokenKind::Identifier) return {};
        std::string joined{peek_at(1).text.data(), peek_at(1).text.size()};
        std::size_t offset = 2;
        while (peek_at(offset).kind == TokenKind::ColonColon && peek_at(offset + 1).kind == TokenKind::Identifier) {
            joined += "::";
            joined += std::string(peek_at(offset + 1).text.data(), peek_at(offset + 1).text.size());
            offset += 2;
        }
        return joined;
    }

    // Consumes a qualified name (`Identifier (:: Identifier)*`) and
    // returns it joined the same way peek_qualified_name does. Only call
    // when the current token is already known to be an Identifier (e.g.
    // right after peek_qualified_name returned non-empty, or after
    // check(TokenKind::Identifier)).
    std::string parse_qualified_name() {
        const Token& first_tok = advance();
        std::string joined{first_tok.text.data(), first_tok.text.size()};
        while (check(TokenKind::ColonColon) && peek_at(1).kind == TokenKind::Identifier) {
            advance(); // ::
            joined += "::";
            const Token& next_tok = advance();
            joined += std::string(next_tok.text.data(), next_tok.text.size());
        }
        return joined;
    }

    [[nodiscard]] std::expected<std::string, ParseError> parse_global_qualified_name() {
        auto colon_result = expect(TokenKind::ColonColon, "'::'");
        if (!colon_result.has_value()) return std::unexpected(std::move(colon_result).error());
        auto ident_result = expect(TokenKind::Identifier, "identifier after '::'");
        if (!ident_result.has_value()) return std::unexpected(std::move(ident_result).error());
        std::string joined{ident_result.value().text.data(), ident_result.value().text.size()};
        while (check(TokenKind::ColonColon) && peek_at(1).kind == TokenKind::Identifier) {
            advance(); // ::
            joined += "::";
            const Token& next_tok = advance();
            joined += std::string(next_tok.text.data(), next_tok.text.size());
        }
        return joined;
    }

    // The following three helpers hold what used to be braced
    // `case TypeKind::X: { ...; return result; }` bodies inside
    // type_to_string() below. They're factored out -- rather than left
    // as nested blocks in the switch -- so each case there can end
    // directly in a bare `return ...;` (scpp's own switch-case grammar,
    // enforced by validate_switch_fallthrough(), requires every
    // non-empty case's own last statement to literally be that, not
    // merely end with one nested inside a further compound statement);
    // it also sidesteps every case independently declaring its own
    // same-named `result` local from colliding in the shared switch
    // scope.
    [[nodiscard]] std::string type_to_string_named(const Type& type, const std::string& const_prefix) const {
        std::string result = const_prefix;
        result += type.name;
        if (!type.template_args.empty()) {
            result += "<";
            for (std::size_t i = 0; i < type.template_args.size(); i++) {
                if (i != 0) result += ", ";
                result += type_to_string(type.template_args[i]);
            }
            result += ">";
        }
        return result;
    }

    [[nodiscard]] std::string type_to_string_function(const Type& type, const std::string& const_prefix) const {
        std::string result{const_prefix};
        result += type_to_string(*type.function_return);
        result += "(";
        for (std::size_t i = 0; i < type.function_params.size(); i++) {
            if (i != 0) result += ", ";
            result += type_to_string(type.function_params[i]);
        }
        result += ")";
        return result;
    }

    [[nodiscard]] std::string type_to_string_function_pointer(const Type& type, const std::string& const_prefix) const {
        std::string result{const_prefix};
        result += type_to_string(*type.function_return);
        result += " (*";
        result += ")(";
        for (std::size_t i = 0; i < type.function_params.size(); i++) {
            if (i != 0) result += ", ";
            result += type_to_string(type.function_params[i]);
        }
        result += ")";
        return result;
    }

    [[nodiscard]] std::string type_to_string(const Type& type) const {
        std::string const_prefix{type.is_const_qualified ? "const " : ""};
        switch (type.kind) {
        case TypeKind::Named: return type_to_string_named(type, const_prefix);
        case TypeKind::Pointer:
            return [&, this]() -> std::string {
                std::string _msg_1557{const_prefix};
                _msg_1557 += (type.is_mutable_pointee ? std::string() : std::string("const "));
                _msg_1557 += type_to_string(*type.pointee);
                _msg_1557 += "*";
                return _msg_1557;
            }();
        case TypeKind::Function: return type_to_string_function(type, const_prefix);
        case TypeKind::FunctionPointer: return type_to_string_function_pointer(type, const_prefix);
        case TypeKind::Array: return [&, this]() -> std::string {
                std::string _msg_1561{const_prefix};
                _msg_1561 += type_to_string(*type.element);
                _msg_1561 += "[";
                _msg_1561 += std::to_string(type.array_size);
                _msg_1561 += "]";
                return _msg_1561;
            }();
        case TypeKind::Reference:
            if (type.is_rvalue_ref) {
                std::string _msg_1563{type_to_string(*type.pointee)};
                _msg_1563 += "&&";
                return _msg_1563;
            }
            return [&, this]() -> std::string {
                std::string _msg_1565{const_prefix};
                _msg_1565 += (type.is_mutable_ref ? std::string() : std::string("const "));
                _msg_1565 += type_to_string(*type.pointee);
                _msg_1565 += "&";
                return _msg_1565;
            }();
        case TypeKind::Span:
            return [&, this]() -> std::string {
                std::string _msg_1567{const_prefix};
                _msg_1567 += std::string("std::span<");
                _msg_1567 += (type.is_mutable_ref ? std::string() : std::string("const "));
                _msg_1567 += type_to_string(*type.pointee);
                _msg_1567 += ">";
                return _msg_1567;
            }();
        }
        return "<unknown-type>";
    }

    void maybe_mark_reference_wrapper_lifetime_source(Type& type) const {
        if (type.kind != TypeKind::Named || type.template_args.size() != 1 || !type.non_type_args.empty()) return;
        if (type.name == "std::reference_wrapper") {
            type.is_reference_wrapper_lifetime_source = true;
            return;
        }
        if (type.name == "std::optional" && type.template_args[0].is_reference_wrapper_lifetime_source) {
            type.is_reference_wrapper_lifetime_source = true;
        }
    }

    [[nodiscard]] std::optional<std::string>
    try_parse_template_static_member_name(const std::string& base_name, bool explicit_global_qualification) {
        if (!check(TokenKind::Less)) return std::optional<std::string>{};
        std::string resolved_base{};
        if (explicit_global_qualification) {
            resolved_base = base_name;
        } else {
            resolved_base = resolve_visible_type_name(base_name);
        }
        if (resolved_base.empty() || !generic_type_names_.contains(resolved_base)) return std::optional<std::string>{};
        std::size_t saved_pos = pos_;
        advance(); // '<'
        std::vector<Type> template_args{};

        // Speculative parse: any failure below means this wasn't actually a
        // template-qualified static member access, so backtrack to
        // saved_pos and report "not a match" via nullopt -- this replaces
        // the try/catch(const ParseError&) that used to swallow the error.
        bool ok = true;
        if (!check(TokenKind::Greater)) {
            while (true) {
                auto arg_result = parse_template_type_argument();
                if (!arg_result.has_value()) { ok = false; break; }
                Type __arg_result_value = std::move(arg_result).value();
                template_args.push_back(std::move(__arg_result_value));
                if (!(ok && match(TokenKind::Comma))) break;
            }
        }
        if (ok) {
            auto gt_result = expect(TokenKind::Greater, "'>'");
            if (!gt_result.has_value()) ok = false;
        }
        if (!ok) {
            pos_ = saved_pos;
            return std::optional<std::string>{};
        }
        if (!match(TokenKind::ColonColon)) {
            pos_ = saved_pos;
            return std::optional<std::string>{};
        }
        auto member_result = expect(TokenKind::Identifier, "member name");
        if (!member_result.has_value()) {
            pos_ = saved_pos;
            return std::optional<std::string>{};
        }
        std::string member_name = std::string(member_result.value().text.data(), member_result.value().text.size());
        std::string result{resolved_base};
        result += "<";
        for (std::size_t i = 0; i < template_args.size(); i++) {
            if (i != 0) result += ", ";
            result += type_to_string(template_args[i]);
        }
        result += ">::";
        result += member_name;
        return std::move(result);
    }

    [[nodiscard]] bool is_exported_generic_type_template(const Program& program, const std::string& name) const {
        for (const StructDef& def : program.structs) {
            if (!def.is_exported || def.name != name) continue;
            if (!def.template_params.empty()) return true;
        }
        for (const ClassDef& def : program.classes) {
            if (!def.is_exported || def.name != name) continue;
            if (!def.template_params.empty() || def.is_variadic_primary_template) return true;
        }
        return false;
    }

    // ch11 §11.4/§11.5: joins `bare_name` onto the current
    // namespace_stack_ prefix, e.g. inside `namespace std { ... }`,
    // qualify_name("string") -> "std::string". Outside any namespace
    // block (namespace_stack_ empty, the overwhelmingly common case
    // today), returns `bare_name` completely unchanged -- so every
    // existing, non-namespaced file's declarations keep exactly the same
    // `.name` they always have.
    [[nodiscard]] std::string qualify_name(const std::string& bare_name) const {
        if (namespace_stack_.empty()) return std::string(bare_name);
        std::string joined{};

        for (const std::string& segment : namespace_stack_) {
            joined += segment;
            joined += "::";
        }
        joined += bare_name;
        return joined;
    }

    [[nodiscard]] std::optional<std::string> resolve_visible_local_type_name(const std::string& spelled_name) const {
        if (spelled_name.empty() || spelled_name.contains("::")) return std::optional<std::string>{};
        // std::vector has no rbegin()/rend() yet -- walk backwards (innermost
        // scope first) by index instead.
        for (std::size_t i = local_type_name_scopes_.size(); i > 0; i--) {
            const std::unordered_map<std::string, std::string>& scope = local_type_name_scopes_[i - 1];
            auto local_it = scope.find(spelled_name);
            if (local_it != scope.end()) {
                [[scpp::unsafe]] {
                    return local_it->second;
                }
            }
        }
        return std::optional<std::string>{};
    }

    // Returns the enclosing type name a member type definition should be
    // qualified with, and clears it so it applies to exactly one
    // definition (see pending_nested_type_owner_'s own declaration).
    // Empty when the definition being parsed is not a member type.
    [[nodiscard]] std::string take_pending_nested_type_owner() {
        std::string owner = pending_nested_type_owner_;
        pending_nested_type_owner_.clear();
        return owner;
    }

    [[nodiscard]] std::string fresh_local_type_name(const std::string& bare_name) {
        {
            std::string _msg_1751{"$local_type_"};
            _msg_1751 += std::to_string(++next_local_type_id_);
            _msg_1751 += "::";
            _msg_1751 += bare_name;
            return _msg_1751;
        }
    }

    [[nodiscard]] std::expected<void, ParseError> register_local_type_name(const std::string& bare_name, const std::string& qualified_name, const SourceLocation& loc) {
        if (local_type_name_scopes_.empty()) {
            return std::unexpected(ParseError(loc.line, loc.column, "internal parser error: missing block scope for local type definition"));
        }
        auto emplace_result = local_type_name_scopes_.back().emplace(bare_name, qualified_name);
        if (!emplace_result.second) {
            {
                std::string _msg_1761{"redeclaration of local type '"};
                _msg_1761 += bare_name;
                _msg_1761 += "' in the same block scope";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_1761));
            }
        }
        return {};
    }

    [[nodiscard]] std::optional<Type> resolve_visible_type_alias(const std::string& spelled_name) const {
        if (spelled_name.empty()) return std::optional<Type>{};
        // Must capture 'this' explicitly (not just '[&]') so the
        // implicit `type_aliases_` -> `this->type_aliases_` member-field
        // rewrite that runs before lambda-capture analysis has an
        // actual 'this' capture to resolve through. Not a compiler bug,
        // as this comment previously claimed, and not "matching real
        // C++" either -- C++ `[&]`/`[=]` *does* capture `this`
        // implicitly (only deprecated for `[=]`, P0806R2). ch05 §5.12
        // deliberately makes scpp stricter: `this` must always be named.
        // This lambda is one of the sites that rule requires, and the
        // rule is now diagnosed rather than silently applied (see
        // resolve_lambda in movecheck/monomorphize.cppm).
        auto lookup = [this](const std::string& candidate) -> std::optional<Type> {
            if (!type_aliases_.contains(candidate)) return std::optional<Type>{};
            return std::optional<Type>{type_aliases_.at(candidate)};
        };
        if (spelled_name.contains("::")) {
            if (std::optional<Type> alias = lookup(spelled_name); alias.has_value()) return alias;
            if (!namespace_stack_.empty()) {
                for (std::size_t depth = namespace_stack_.size(); depth > 0; depth--) {
                    std::string candidate{};

                    for (std::size_t i = 0; i < depth; i++) {
                        candidate += namespace_stack_[i];
                        candidate += "::";
                    }
                    candidate += spelled_name;
                    if (std::optional<Type> alias = lookup(candidate); alias.has_value()) return alias;
                }
            }
            return std::optional<Type>{};
        }
        if (std::optional<Type> alias = lookup(spelled_name); alias.has_value()) return alias;
        if (!namespace_stack_.empty()) {
            for (std::size_t depth = namespace_stack_.size(); depth > 0; depth--) {
                std::string candidate{};

                for (std::size_t i = 0; i < depth; i++) {
                    candidate += namespace_stack_[i];
                    candidate += "::";
                }
                candidate += spelled_name;
                if (std::optional<Type> alias = lookup(candidate); alias.has_value()) return alias;
            }
        }
        return std::optional<Type>{};
    }

    [[nodiscard]] std::string resolve_visible_type_name(const std::string& spelled_name) const {
        if (spelled_name.empty()) return {};
        if (std::optional<std::string> local = resolve_visible_local_type_name(spelled_name); local.has_value()) {
            return std::move(local).value();
        }
        auto alias_underlying_name = [&](const Type& type) -> std::string {
            if (type.kind != TypeKind::Named || !type.template_args.empty() || !type.non_type_args.empty() ||
                type.is_pack_expansion) {
                return {};
            }
            return std::string(type.name);
        };
        if (spelled_name.contains("::")) {
            if (struct_names_.contains(spelled_name)) return std::string(spelled_name);
            if (!namespace_stack_.empty()) {
                for (std::size_t depth = namespace_stack_.size(); depth > 0; depth--) {
                    std::string candidate{};

                    for (std::size_t i = 0; i < depth; i++) {
                        candidate += namespace_stack_[i];
                        candidate += "::";
                    }
                    candidate += spelled_name;
                    if (struct_names_.contains(candidate)) return candidate;
                }
            }
            if (std::optional<Type> alias = resolve_visible_type_alias(spelled_name); alias.has_value()) {
                return alias_underlying_name(*alias);
            }
            return {};
        }
        if (struct_names_.contains(spelled_name)) return std::string(spelled_name);
        if (!namespace_stack_.empty()) {
            for (std::size_t depth = namespace_stack_.size(); depth > 0; depth--) {
                std::string candidate{};

                for (std::size_t i = 0; i < depth; i++) {
                    candidate += namespace_stack_[i];
                    candidate += "::";
                }
                candidate += spelled_name;
                if (struct_names_.contains(candidate)) return candidate;
            }
        }
        if (std::optional<Type> alias = resolve_visible_type_alias(spelled_name); alias.has_value()) {
            return alias_underlying_name(*alias);
        }
        return {};
    }

    [[nodiscard]] bool is_visible_type_name(const std::string& spelled_name) const {
        return !resolve_visible_type_name(spelled_name).empty() || resolve_visible_type_alias(spelled_name).has_value();
    }

    // ch05 §5.14/spec §13.2: resolves a (possibly namespace-qualified)
    // spelled concept name to the fully-qualified name it was declared
    // under (concept_names_ always stores a concept's own qualify_name'd
    // name, mirroring struct_names_ for a class/struct) -- the same
    // progressive-namespace-prefix search resolve_visible_type_name
    // above already uses for a type name, just checked against
    // concept_names_ instead of struct_names_. Returns an empty string
    // when no visible concept matches. Currently only consulted by
    // parse_optional_method_requires_clause (a per-method `requires
    // Concept<T>` clause, ch05 §5.14) -- the abbreviated `Concept auto`
    // parameter form and the full `template<Concept T>` header
    // (parse_param_type/parse_generic_type_header) still only accept an
    // unqualified concept name, unchanged; extending those too is
    // orthogonal to this fix's own scope.
    [[nodiscard]] std::string resolve_visible_concept_name(const std::string& spelled_name) const {
        if (spelled_name.empty()) return {};
        if (concept_names_.contains(spelled_name)) return std::string(spelled_name);
        if (!namespace_stack_.empty()) {
            for (std::size_t depth = namespace_stack_.size(); depth > 0; depth--) {
                std::string candidate{};

                for (std::size_t i = 0; i < depth; i++) {
                    candidate += namespace_stack_[i];
                    candidate += "::";
                }
                candidate += spelled_name;
                if (concept_names_.contains(candidate)) return candidate;
            }
        }
        return {};
    }

    [[nodiscard]] const InjectedGenericTypeName* find_injected_generic_type_name(const std::string& spelled_name) const {
        // std::vector has no rbegin()/rend() yet -- walk backwards by index.
        for (std::size_t i = injected_generic_type_name_stack_.size(); i > 0; i--) {
            const InjectedGenericTypeName& entry = injected_generic_type_name_stack_[i - 1];
            if (entry.spelled_name == spelled_name) return &entry;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<std::string>
    resolve_static_member_owner_name(const std::string& spelled_owner, bool explicit_global_qualification) const {
        std::string resolved_owner{};
        if (explicit_global_qualification) {
            resolved_owner = spelled_owner;
        } else {
            resolved_owner = resolve_visible_type_name(spelled_owner);
        }
        if (resolved_owner.empty() || !class_names_.contains(resolved_owner)) return std::optional<std::string>{};
        return resolved_owner;
    }

    [[nodiscard]] std::optional<std::string>
    resolve_value_qualified_type_owner_name(const std::string& spelled_name, bool explicit_global_qualification) const {
        std::size_t last_separator = spelled_name.rfind("::");
        if (last_separator == static_cast<std::size_t>(-1)) return std::optional<std::string>{};
        std::string owner_name = spelled_name.substr(static_cast<std::size_t>(0), last_separator);
        std::string member_name = spelled_name.substr(last_separator + 2);
        if (owner_name.empty() || member_name.empty()) return std::optional<std::string>{};
        std::string resolved_owner{};
        if (explicit_global_qualification) {
            resolved_owner = owner_name;
        } else {
            resolved_owner = resolve_visible_type_name(owner_name);
        }
        if (resolved_owner.empty() || resolved_owner == owner_name) return std::optional<std::string>{};
        {
            std::string _msg_1927{resolved_owner};
            _msg_1927 += "::";
            _msg_1927 += member_name;
            return _msg_1927;
        }
    }

    // Type identity is scpp::types_equal (scpp.ast). The parser used to
    // define its own member copy; it and driver.cppm's
    // types_equal_for_payload_merge were the only two of the five that
    // compared non_type_args by value, which is what the shared one now
    // does everywhere.

    [[nodiscard]] bool params_equal(const std::vector<Param>& a, const std::vector<Param>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (a[i].name != b[i].name || a[i].generic_concept != b[i].generic_concept ||
                a[i].is_parameter_pack != b[i].is_parameter_pack ||
                a[i].require_thread_movable != b[i].require_thread_movable ||
                a[i].require_thread_shareable != b[i].require_thread_shareable ||
                !types_equal(a[i].type, b[i].type)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool lifetime_annotations_equivalent(const Function& a, const Function& b) const {
        if (a.params.size() != b.params.size()) return false;
        std::unordered_map<std::string, std::string> a_to_b{};

        std::unordered_map<std::string, std::string> b_to_a{};

        auto names_equivalent = [&](const LifetimeAnnotation& lhs, const LifetimeAnnotation& rhs) {
            if (lhs.present() != rhs.present()) return false;
            if (!lhs.present()) return true;
            if (lhs.is_any() || rhs.is_any()) return lhs.is_any() && rhs.is_any();
            bool lhs_found = a_to_b.contains(lhs.name);
            bool rhs_found = b_to_a.contains(rhs.name);
            if (lhs_found || rhs_found) {
                return lhs_found && rhs_found && a_to_b.at(lhs.name) == rhs.name && b_to_a.at(rhs.name) == lhs.name;
            }
            a_to_b.emplace(lhs.name, rhs.name);
            b_to_a.emplace(rhs.name, lhs.name);
            return true;
        };
        for (std::size_t i = 0; i < a.params.size(); i++) {
            if (!names_equivalent(a.params[i].lifetime, b.params[i].lifetime)) return false;
        }
        return names_equivalent(a.return_lifetime, b.return_lifetime);
    }

    [[nodiscard]] bool same_function_signature(const Function& a, const Function& b) const {
        return a.name == b.name && types_equal(a.return_type, b.return_type) && params_equal(a.params, b.params) &&
               lifetime_annotations_equivalent(a, b) &&
               a.has_varargs == b.has_varargs && a.is_extern_c == b.is_extern_c &&
               a.is_module_extern == b.is_module_extern && a.is_unsafe == b.is_unsafe &&
               a.eval_mode == b.eval_mode && a.receiver_ref_qualifier == b.receiver_ref_qualifier &&
               a.is_static == b.is_static && a.access == b.access && a.member_owner_class == b.member_owner_class &&
               a.is_virtual == b.is_virtual && a.is_override == b.is_override && a.is_pure == b.is_pure &&
               a.is_defaulted == b.is_defaulted && a.is_deleted == b.is_deleted;
    }

    [[nodiscard]] bool same_template_param_shape(const std::vector<GenericTypeParam>& a,
                                                 const std::vector<GenericTypeParam>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (a[i].name != b[i].name || a[i].concept_name != b[i].concept_name ||
                a[i].is_non_type != b[i].is_non_type || a[i].is_pack != b[i].is_pack ||
                !types_equal(a[i].non_type_type, b[i].non_type_type)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool same_non_type_expr(const std::shared_ptr<Expr>& a, const std::shared_ptr<Expr>& b) const {
        if ((a != nullptr) != (b != nullptr)) return false;
        if (a == nullptr) return true;
        return a->kind == b->kind && a->int_value == b->int_value && a->name == b->name;
    }

    [[nodiscard]] bool same_base_specifiers(const std::vector<BaseSpecifier>& a, const std::vector<BaseSpecifier>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (!types_equal(a[i].base_type, b[i].base_type) || a[i].access != b[i].access ||
                a[i].is_virtual != b[i].is_virtual || a[i].kind != b[i].kind ||
                a[i].pack_arg_name != b[i].pack_arg_name) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool same_using_declarations(const std::vector<ClassUsingDeclaration>& a,
                                               const std::vector<ClassUsingDeclaration>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (a[i].base_name != b[i].base_name || a[i].member_name != b[i].member_name || a[i].access != b[i].access) {
                return false;
            }
        }
        return true;
    }


    [[nodiscard]] bool imported_function_body_must_stay_available(const Program& imported, const Function& fn) const {
        if (fn.is_compile_time_dependency) return true;
        if (fn.is_generic_template || fn.eval_mode != FunctionEvalMode::RuntimeOnly) return true;
        if (!fn.member_owner_class.empty()) {
            std::string owner_name = fn.member_owner_class;
            if (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.pointee != nullptr) {
                owner_name = fn.params[0].type.pointee->name;
            }
            if (is_exported_generic_type_template(imported, owner_name)) return true;
        }
        return false;
    }

    void collect_hidden_function_designators_in_expr(const Expr& expr, std::unordered_set<std::string>& out) const {
        if (expr.kind == ExprKind::Identifier && !expr.explicit_template_args.empty()) {
            out.insert(expr.name);
        }
        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs != nullptr &&
            expr.lhs->kind == ExprKind::Identifier && !expr.lhs->explicit_template_args.empty()) {
            out.insert(expr.lhs->name);
        }
        if (expr.lhs != nullptr) collect_hidden_function_designators_in_expr(*expr.lhs, out);
        if (expr.rhs != nullptr) collect_hidden_function_designators_in_expr(*expr.rhs, out);
        if (expr.third != nullptr) collect_hidden_function_designators_in_expr(*expr.third, out);
        for (const ExprPtr& arg : expr.args) {
            if (arg != nullptr) collect_hidden_function_designators_in_expr(*arg, out);
        }
    }

    void collect_hidden_function_designators_in_stmt(const Stmt& stmt, std::unordered_set<std::string>& out) const {
        switch (stmt.kind) {
            case StmtKind::Block:
                for (const StmtPtr& child : stmt.statements) {
                    if (child != nullptr) collect_hidden_function_designators_in_stmt(*child, out);
                }
                return;
            case StmtKind::VarDecl:
                if (stmt.init != nullptr) collect_hidden_function_designators_in_expr(*stmt.init, out);
                for (const ExprPtr& arg : stmt.ctor_args) {
                    if (arg != nullptr) collect_hidden_function_designators_in_expr(*arg, out);
                }
                return;
            case StmtKind::ExprStmt:
            case StmtKind::Return:
                if (stmt.expr != nullptr) collect_hidden_function_designators_in_expr(*stmt.expr, out);
                return;
            case StmtKind::If:
                if (stmt.condition != nullptr) collect_hidden_function_designators_in_expr(*stmt.condition, out);
                if (stmt.then_branch != nullptr) collect_hidden_function_designators_in_stmt(*stmt.then_branch, out);
                if (stmt.else_branch != nullptr) collect_hidden_function_designators_in_stmt(*stmt.else_branch, out);
                return;
            case StmtKind::While:
                if (stmt.condition != nullptr) collect_hidden_function_designators_in_expr(*stmt.condition, out);
                if (stmt.then_branch != nullptr) collect_hidden_function_designators_in_stmt(*stmt.then_branch, out);
                return;
            case StmtKind::Switch:
                if (stmt.condition != nullptr) collect_hidden_function_designators_in_expr(*stmt.condition, out);
                for (const SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value != nullptr) collect_hidden_function_designators_in_expr(*switch_case.value, out);
                    for (const StmtPtr& child : switch_case.statements) {
                        if (child != nullptr) collect_hidden_function_designators_in_stmt(*child, out);
                    }
                }
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough:
                return;
        }
    }

    [[nodiscard]] std::unordered_set<std::string> imported_hidden_function_designators(const Program& imported) const {
        std::unordered_set<std::string> out{};

        for (const Function& fn : imported.functions) {
            if (!imported_function_body_must_stay_available(imported, fn) || fn.body == nullptr) continue;
            collect_hidden_function_designators_in_stmt(*fn.body, out);
        }
        return out;
    }

    [[nodiscard]] bool same_specialization_args(const std::vector<Type>& a, const std::vector<Type>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (!types_equal(a[i], b[i])) return false;
        }
        return true;
    }

    // std::vector<std::string> has no built-in operator==/!= here yet --
    // a plain element-by-element walk, exactly like same_specialization_args
    // just above (and the file's other same_*-named identity helpers).
    [[nodiscard]] bool same_namespace_path(const std::vector<std::string>& a, const std::vector<std::string>& b) const {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); i++) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    [[nodiscard]] bool same_enum_identity(const EnumDef& a, const EnumDef& b) const {
        if (a.name != b.name || !same_namespace_path(a.namespace_path, b.namespace_path) || !types_equal(a.underlying_type, b.underlying_type) ||
            a.variants.size() != b.variants.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.variants.size(); i++) {
            if (a.variants[i].name != b.variants[i].name || a.variants[i].value != b.variants[i].value) return false;
        }
        return true;
    }

    [[nodiscard]] bool same_struct_identity(const StructDef& a, const StructDef& b) const {
        if (a.name != b.name || !same_namespace_path(a.namespace_path, b.namespace_path) || a.is_union != b.is_union ||
            a.is_forward_declaration != b.is_forward_declaration ||
            a.is_packed != b.is_packed || a.thread_movable_override != b.thread_movable_override ||
            a.thread_shareable_override != b.thread_shareable_override || a.is_nodiscard != b.is_nodiscard ||
            a.nodiscard_reason != b.nodiscard_reason ||
            !same_template_param_shape(a.template_params, b.template_params) || a.fields.size() != b.fields.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.fields.size(); i++) {
            if (a.fields[i].name != b.fields[i].name || !types_equal(a.fields[i].type, b.fields[i].type)) return false;
        }
        return true;
    }

    [[nodiscard]] static std::string_view record_tag_keyword(RecordTagKind kind) {
        switch (kind) {
            case RecordTagKind::Struct: return "struct";
            case RecordTagKind::Class: return "class";
            case RecordTagKind::Union: return "union";
        }
        return "record";
    }

    [[nodiscard]] std::expected<void, ParseError> register_record_tag_kind(const std::string& name, RecordTagKind kind, const SourceLocation& loc) {
        if (record_tag_kinds_.contains(name)) {
            RecordTagKind existing_kind = record_tag_kinds_.at(name);
            if (existing_kind != kind) {
                {
                    std::string _msg_2195{"'"};
                    _msg_2195 += name;
                    _msg_2195 += "' was previously declared as ";
                    _msg_2195 += std::string(record_tag_keyword(existing_kind).data(), record_tag_keyword(existing_kind).size());
                    _msg_2195 += " and cannot later be declared as ";
                    _msg_2195 += std::string(record_tag_keyword(kind).data(), record_tag_keyword(kind).size());
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_2195));
                }
            }
            return {};
        }
        record_tag_kinds_.emplace(name, kind);
        return {};
    }

    [[nodiscard]] bool exported_forward_struct_exists(const Program& program, const std::string& name) const {
        for (const StructDef& def : program.structs) {
            if (def.name == name && def.is_forward_declaration && def.is_exported) return true;
        }
        return false;
    }

    [[nodiscard]] bool exported_forward_class_exists(const Program& program, const std::string& name) const {
        for (const ClassDef& def : program.classes) {
            if (def.name == name && def.is_forward_declaration && def.is_exported &&
                def.template_params.empty() && !def.is_variadic_primary_template && !def.is_variadic_specialization &&
                !def.is_partial_specialization) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool same_class_identity(const ClassDef& a, const ClassDef& b) const {
        if (a.name != b.name || !same_namespace_path(a.namespace_path, b.namespace_path) || a.is_concept_witness != b.is_concept_witness ||
            a.is_forward_declaration != b.is_forward_declaration ||
            a.is_synthetic_check_only != b.is_synthetic_check_only || a.is_interface != b.is_interface ||
            !same_base_specifiers(a.base_specifiers, b.base_specifiers) ||
            !same_using_declarations(a.using_declarations, b.using_declarations) ||
            a.is_variadic_primary_template != b.is_variadic_primary_template ||
            a.is_variadic_specialization != b.is_variadic_specialization ||
            a.is_partial_specialization != b.is_partial_specialization || a.thread_movable_override != b.thread_movable_override ||
            a.thread_shareable_override != b.thread_shareable_override || a.is_nodiscard != b.is_nodiscard ||
            a.nodiscard_reason != b.nodiscard_reason || !same_specialization_args(a.specialization_template_args, b.specialization_template_args) ||
            !same_template_param_shape(a.template_params, b.template_params) || a.fields.size() != b.fields.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.fields.size(); i++) {
            if (a.fields[i].name != b.fields[i].name || a.fields[i].access != b.fields[i].access ||
                !types_equal(a.fields[i].type, b.fields[i].type)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static std::string join_namespace_path(const std::vector<std::string>& namespace_path) {
        std::string joined{};

        for (std::size_t i = 0; i < namespace_path.size(); i++) {
            if (i != 0) joined += "::";
            joined += namespace_path[i];
        }
        return joined;
    }

    [[nodiscard]] bool parsing_compiled_module_interface() const {
        return source_path_ != nullptr && source_path_->size() >= 6 &&
               source_path_->substr(source_path_->size() - 6) == ".scppm";
    }

    [[nodiscard]] bool is_bodyless_free_function_forward_decl(const Function& fn) const {
        // [dcl.fct.def.delete]/1: `= delete` *is* a definition, so a
        // deleted free function is never a forward declaration awaiting
        // one (and, by /4, must be the first declaration).
        return !parsing_compiled_module_interface() && fn.body == nullptr && fn.owning_module.empty() && !fn.is_extern_c &&
               !fn.is_module_extern && !fn.is_deleted && (fn.params.empty() || fn.params[0].name != "this");
    }

    [[nodiscard]] bool is_bodyless_member_forward_decl(const Function& fn) const {
        return fn.expects_out_of_line_definition && fn.body == nullptr && !fn.member_owner_class.empty() && !fn.is_pure &&
               !fn.is_defaulted && !fn.is_deleted;
    }

    [[nodiscard]] static bool is_bodyless_extern_c_declaration(const Function& fn) {
        return fn.is_extern_c && fn.body == nullptr;
    }

    [[nodiscard]] bool same_function_declarator(const Function& a, const Function& b) const {
        return a.name == b.name && params_equal(a.params, b.params) && a.has_varargs == b.has_varargs &&
               a.member_owner_class == b.member_owner_class && a.receiver_ref_qualifier == b.receiver_ref_qualifier &&
               a.is_static == b.is_static && a.access == b.access &&
               same_template_param_shape(a.template_params, b.template_params);
    }

    // ParsedOutOfLineMemberOwner/OutOfLineMemberKind/
    // ParsedOutOfLineMemberDefinition are now file-scope types (see
    // their own comment above this Parser struct) -- hoisted purely
    // for self-hosting reasons, no behavior change.

    [[nodiscard]] std::string out_of_line_member_suffix(OutOfLineMemberKind kind, std::string_view member_name) const {
        switch (kind) {
            case OutOfLineMemberKind::Constructor: return "_new";
            case OutOfLineMemberKind::Destructor: return "_delete";
            case OutOfLineMemberKind::Method: return [&]() -> std::string {
                    std::string _msg_2288{"_"};
                    _msg_2288 += std::string(member_name.data(), member_name.size());
                    return _msg_2288;
                }();
            case OutOfLineMemberKind::OperatorDeref: return "_operator_deref";
            case OutOfLineMemberKind::OperatorArrow: return "_operator_arrow";
            case OutOfLineMemberKind::OperatorEqual: return "_operator_equal";
            case OutOfLineMemberKind::OperatorNotEqual: return "_operator_not_equal";
            case OutOfLineMemberKind::OperatorAssign: return "_operator_assign";
        }
        return {};
    }

    [[nodiscard]] std::string out_of_line_member_display_name(const ParsedOutOfLineMemberDefinition& parsed) const {
        switch (parsed.kind) {
            case OutOfLineMemberKind::Constructor:
                return [&]() -> std::string {
                    std::string _msg_2301{parsed.owner.spelled_name};
                    _msg_2301 += "::";
                    _msg_2301 += parsed.owner.unqualified_name;
                    return _msg_2301;
                }();
            case OutOfLineMemberKind::Destructor:
                return [&]() -> std::string {
                    std::string _msg_2303{parsed.owner.spelled_name};
                    _msg_2303 += "::~";
                    _msg_2303 += parsed.owner.unqualified_name;
                    return _msg_2303;
                }();
            case OutOfLineMemberKind::Method:
                return [&]() -> std::string {
                    std::string _msg_2305{parsed.owner.spelled_name};
                    _msg_2305 += "::";
                    _msg_2305 += parsed.member_name;
                    return _msg_2305;
                }();
            case OutOfLineMemberKind::OperatorDeref: return [&]() -> std::string {
                    std::string _msg_2306{parsed.owner.spelled_name};
                    _msg_2306 += "::operator*";
                    return _msg_2306;
                }();
            case OutOfLineMemberKind::OperatorArrow: return [&]() -> std::string {
                    std::string _msg_2307{parsed.owner.spelled_name};
                    _msg_2307 += "::operator->";
                    return _msg_2307;
                }();
            case OutOfLineMemberKind::OperatorEqual: return [&]() -> std::string {
                    std::string _msg_2308{parsed.owner.spelled_name};
                    _msg_2308 += "::operator==";
                    return _msg_2308;
                }();
            case OutOfLineMemberKind::OperatorNotEqual: return [&]() -> std::string {
                    std::string _msg_2309{parsed.owner.spelled_name};
                    _msg_2309 += "::operator!=";
                    return _msg_2309;
                }();
            case OutOfLineMemberKind::OperatorAssign: return [&]() -> std::string {
                    std::string _msg_2310{parsed.owner.spelled_name};
                    _msg_2310 += "::operator=";
                    return _msg_2310;
                }();
        }
        return std::string(parsed.owner.spelled_name);
    }

    [[nodiscard]] std::optional<ParsedOutOfLineMemberOwner>
    parse_out_of_line_member_owner() {
        bool explicit_global = check(TokenKind::ColonColon);
        std::size_t offset = static_cast<std::size_t>(explicit_global ? 1 : 0);
        if (peek_at(offset).kind != TokenKind::Identifier) return std::optional<ParsedOutOfLineMemberOwner>{};

        std::vector<std::string> segments{};

        segments.push_back(std::string(peek_at(offset).text.data(), peek_at(offset).text.size()));
        std::size_t look = offset + 1;
        while (peek_at(look).kind == TokenKind::ColonColon && peek_at(look + 1).kind == TokenKind::Identifier) {
            segments.push_back(std::string(peek_at(look + 1).text.data(), peek_at(look + 1).text.size()));
            look += 2;
        }

        for (std::size_t prefix_len = segments.size(); prefix_len > 0; prefix_len--) {
            std::string spelled_name{};

            for (std::size_t i = 0; i < prefix_len; i++) {
                if (i != 0) spelled_name += "::";
                spelled_name += segments[i];
            }
            std::string resolved_name{};
            if (explicit_global) {
                resolved_name = spelled_name;
            } else {
                resolved_name = resolve_visible_type_name(spelled_name);
            }
            if (resolved_name.empty()) continue;
            std::size_t prefix_span = 2 * prefix_len;
            std::size_t next_offset = offset + (prefix_span - 1);
            if (peek_at(next_offset).kind != TokenKind::ColonColon) continue;
            TokenKind member_start = peek_at(next_offset + 1).kind;
            if (member_start != TokenKind::Identifier && member_start != TokenKind::Tilde) continue;

            if (explicit_global) advance();
            // These expect() calls are provably infallible here: every
            // token position they consume was already verified via
            // peek_at(...) lookahead above (the leading Identifier at
            // `offset`, and each ColonColon/Identifier pair making up
            // `segments`), with no intervening advance() other than the
            // single one for an explicit leading '::' just above, which
            // is itself accounted for in `offset`. `.value()` documents
            // that -- unlike a real fallible call, there is deliberately
            // no propagation path here to keep this function's own
            // std::optional-based "no match" contract unchanged.
            expect(TokenKind::Identifier, "record name").value();
            for (std::size_t i = 1; i < prefix_len; i++) {
                expect(TokenKind::ColonColon, "'::'").value();
                expect(TokenKind::Identifier, "identifier after '::'").value();
            }
            ParsedOutOfLineMemberOwner owner{};

            owner.spelled_name = std::move(spelled_name);
            owner.resolved_name = std::move(resolved_name);
            owner.unqualified_name = segments[prefix_len - 1];
            return owner;
        }
        return std::optional<ParsedOutOfLineMemberOwner>{};
    }

    Function build_comparable_out_of_line_member_function(const Function& declared,
                                                          const ParsedOutOfLineMemberDefinition& parsed) {
        Function comparable{};

        comparable.loc = parsed.fn.loc;
        comparable.return_type = parsed.fn.return_type;
        comparable.has_varargs = parsed.fn.has_varargs;
        comparable.eval_mode = parsed.fn.eval_mode;
        comparable.receiver_ref_qualifier = parsed.fn.receiver_ref_qualifier;
        comparable.return_lifetime = parsed.fn.return_lifetime;
        comparable.is_defaulted = parsed.fn.is_defaulted;
        comparable.is_deleted = parsed.fn.is_deleted;
        comparable.params = parsed.fn.params;
        comparable.name = declared.name;
        comparable.member_owner_class = declared.member_owner_class;
        comparable.is_extern_c = declared.is_extern_c;
        comparable.is_module_extern = declared.is_module_extern;
        if (!comparable.is_unsafe) comparable.is_unsafe = declared.is_unsafe;
        if (!comparable.is_nodiscard) {
            comparable.is_nodiscard = declared.is_nodiscard;
            comparable.nodiscard_reason = declared.nodiscard_reason;
        }
        if (comparable.eval_mode == FunctionEvalMode::RuntimeOnly) comparable.eval_mode = declared.eval_mode;
        comparable.is_static = declared.is_static;
        comparable.access = declared.access;
        comparable.is_virtual = declared.is_virtual;
        comparable.is_override = declared.is_override;
        comparable.template_params = declared.template_params;
        comparable.is_generic_template = declared.is_generic_template;
        if (comparable.method_requires_concept.empty()) comparable.method_requires_concept = declared.method_requires_concept;
        if (!comparable.return_lifetime.present()) comparable.return_lifetime = declared.return_lifetime;
        comparable.namespace_path = declared.namespace_path;
        comparable.is_exported = declared.is_exported;
        comparable.generic_method_owner_id = declared.generic_method_owner_id;

        std::vector<Param> user_params = std::move(comparable.params);
        comparable.params = std::vector<Param>{};
        std::size_t declared_user_offset = 0;
        if (parsed.kind == OutOfLineMemberKind::Constructor || parsed.kind == OutOfLineMemberKind::Destructor ||
            !declared.is_static) {
            comparable.params.push_back(make_this_param(declared.member_owner_class, parsed.is_const_method));
            declared_user_offset = 1;
        }
        for (std::size_t i = 0; i < user_params.size(); i++) {
            Param& user_params_i_ref = user_params[i];
            Param user = std::move(user_params_i_ref);
            if (declared_user_offset + i < declared.params.size()) {
                const Param& declared_param = declared.params[declared_user_offset + i];
                if (user.generic_concept.empty()) user.generic_concept = declared_param.generic_concept;
                if (!user.lifetime.present()) user.lifetime = declared_param.lifetime;
                if (!user.require_thread_movable) user.require_thread_movable = declared_param.require_thread_movable;
                if (!user.require_thread_shareable) user.require_thread_shareable = declared_param.require_thread_shareable;
            }
            comparable.params.push_back(std::move(user));
        }
        return comparable;
    }

    void merge_out_of_line_member_definition_into(Function& declared, ParsedOutOfLineMemberDefinition parsed) {
        declared.loc = parsed.fn.loc;
        StmtPtr& parsed_body_ref = parsed.fn.body;
        declared.body = std::move(parsed_body_ref);
        std::vector<MemberInitializer>& parsed_member_initializers_ref = parsed.fn.member_initializers;
        declared.member_initializers = std::move(parsed_member_initializers_ref);
        if (parsed.fn.is_defaulted) declared.is_defaulted = true;
        if (parsed.fn.is_deleted) declared.is_deleted = true;
        declared.expects_out_of_line_definition = false;
    }

    // Was a local `try_finish` lambda inside parse_out_of_line_member_
    // definition. Three helper closures there (this one,
    // parse_out_of_line_member_body_or_default,
    // parse_out_of_line_member_eval_mode) all needed to stay alive/
    // reusable across that function's several later, mutually-exclusive
    // branches -- but each closure capturing 'this' by (implicitly
    // mutable) reference in a *named* variable persists its borrow for
    // the rest of the enclosing function (ch05 §5.12: v0.1 has no
    // liveness analysis for a class-typed local), so 3 such named
    // closures simultaneously in scope is rejected as passing 'this' by
    // mutable reference more than once. Ordinary private member
    // functions need no capture/borrow at all, so they sidestep this
    // entirely -- `program`, captured before only via the enclosing
    // function's own by-reference blanket capture, is now an explicit
    // parameter instead.
    [[nodiscard]] std::expected<bool, ParseError> finish_out_of_line_member_definition(Program& program,
                                                                        ParsedOutOfLineMemberDefinition parsed) {
        // [dcl.fct.def.delete]/4: a deleted definition shall be the
        // first declaration of the function. Asked here, before the
        // signature search below, because the generic
        // "does not match its earlier declaration exactly" answer that
        // search would otherwise give answers a question the user did
        // not ask -- the declarator *does* match; being out of line is
        // the whole problem.
        if (parsed.fn.is_deleted) {
            std::string message{"'= delete' on the out-of-line definition of member '"};
            message += out_of_line_member_display_name(parsed);
            message += "': a deleted definition shall be the first declaration of the function ";
            message += "([dcl.fct.def.delete]/4) -- write '= delete' on the declaration inside the class instead";
            return std::unexpected(ParseError(parsed.fn.loc.line, parsed.fn.loc.column, message));
        }
        std::string expected_suffix = out_of_line_member_suffix(parsed.kind, parsed.member_name);
        Function* exact_match = nullptr;
        bool saw_mismatch = false;
        for (Function& candidate : program.functions) {
            if (!is_bodyless_member_forward_decl(candidate) || candidate.member_owner_class != parsed.owner.resolved_name) {
                continue;
            }
            if (!candidate.name.ends_with(expected_suffix)) continue;
            Function comparable = build_comparable_out_of_line_member_function(candidate, parsed);
            auto validate_result = validate_defaulted_special_member(comparable, parsed.fn.loc);
            if (!validate_result.has_value()) return std::unexpected(std::move(validate_result).error());
            if (same_function_signature(candidate, comparable)) {
                exact_match = &candidate;
                break;
            }
            if (same_function_declarator(candidate, comparable)) saw_mismatch = true;
        }
        if (exact_match == nullptr) {
            if (saw_mismatch) {
                // Built via += (not a single chained + expression)
                // -- scpp's dataflow validation only special-cases
                // string concatenation for AddAssign, not plain
                // Add, so a raw string literal chained with a
                // std::string-returning call via '+' is misread as
                // pointer arithmetic (ch06).
                std::string message{"out-of-line definition of member '"};
                message += out_of_line_member_display_name(parsed);
                message += "' does not match its earlier declaration exactly";
                return std::unexpected(ParseError(parsed.fn.loc.line, parsed.fn.loc.column, message));
            }
            std::string message{"out-of-line definition of member '"};
            message += out_of_line_member_display_name(parsed);
            message += "' requires an earlier class/struct member declaration";
            return std::unexpected(ParseError(parsed.fn.loc.line, parsed.fn.loc.column, message));
        }
        // exact_match is a raw Function* known non-null here (the
        // `exact_match == nullptr` branch above always returns), but
        // self-hosting still requires an explicit `[[scpp::unsafe]] { }`
        // to dereference any raw pointer (ch01 §1.3/ch02).
        [[scpp::unsafe]] {
                merge_out_of_line_member_definition_into(*exact_match, std::move(parsed));
        }
        return true;
    }

    // Was a local `parse_body_or_default` lambda -- see
    // finish_out_of_line_member_definition's comment above for why.
    [[nodiscard]] std::expected<void, ParseError> parse_out_of_line_member_body_or_default(Function& fn, const char* entity) {
        if (match(TokenKind::Assign)) {
            return parse_deleted_defaulted_or_pure_suffix(fn, /*allow_default=*/true, /*allow_pure=*/false, entity);
        }
        if (match(TokenKind::Semicolon)) {
            fn.body = nullptr;
            return {};
        }
        auto block_result = parse_block();
        if (!block_result.has_value()) return std::unexpected(std::move(block_result).error());
        fn.body = std::move(block_result).value();
        return {};
    }

    // Was a local `parse_eval_mode` lambda -- see
    // finish_out_of_line_member_definition's comment above for why.
    [[nodiscard]] std::expected<bool, ParseError> parse_out_of_line_member_eval_mode(Function& fn) {
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConstexpr)) {
            advance();
            fn.eval_mode = FunctionEvalMode::Constexpr;
            return true;
        }
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConsteval)) {
            advance();
            fn.eval_mode = FunctionEvalMode::Consteval;
            return true;
        }
        if ((check(TokenKind::KwConstexpr) || check(TokenKind::KwConsteval)) &&
            fn.eval_mode != FunctionEvalMode::RuntimeOnly) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "a declaration may specify at most one of 'constexpr' or 'consteval'"));
        }
        return false;
    }

    [[nodiscard]] std::expected<bool, ParseError> parse_out_of_line_member_definition(Program& program, SourceLocation loc, bool is_unsafe = false,
                                             bool is_nodiscard = false, const std::string& nodiscard_reason = {}) {
        std::size_t saved_pos = pos_;

        Function prefix{};

        prefix.loc = loc;
        prefix.is_unsafe = is_unsafe;
        prefix.is_nodiscard = is_nodiscard;
        prefix.nodiscard_reason = nodiscard_reason;
        while (true) {
            auto eval_mode_result = parse_out_of_line_member_eval_mode(prefix);
            if (!eval_mode_result.has_value()) return std::unexpected(std::move(eval_mode_result).error());
            if (!eval_mode_result.value()) break;
        }

        std::size_t after_prefix = pos_;
        if (std::optional<ParsedOutOfLineMemberOwner> owner = parse_out_of_line_member_owner(); owner.has_value()) {
            auto colon_result = expect(TokenKind::ColonColon, "'::'");
            if (!colon_result.has_value()) return std::unexpected(std::move(colon_result).error());
            if (match(TokenKind::Tilde)) {
                auto name_tok_result = expect(TokenKind::Identifier, "destructor name");
                if (!name_tok_result.has_value()) return std::unexpected(std::move(name_tok_result).error());
                Token name_tok = name_tok_result.value();
                if (name_tok.text != owner->unqualified_name) {
                    {
                        std::string _msg_2553{"destructor name '~"};
                        _msg_2553 += std::string(name_tok.text.data(), name_tok.text.size());
                        _msg_2553 += "' must match the declaring class/struct name '";
                        _msg_2553 += owner->unqualified_name;
                        _msg_2553 += "'";
                        return std::unexpected(ParseError(name_tok.line, name_tok.column,
                                     _msg_2553));
                    }
                }
                ParsedOutOfLineMemberDefinition parsed{};

                parsed.fn.loc = prefix.loc;
                parsed.fn.is_unsafe = prefix.is_unsafe;
                parsed.fn.is_nodiscard = prefix.is_nodiscard;
                parsed.fn.nodiscard_reason = prefix.nodiscard_reason;
                parsed.fn.eval_mode = prefix.eval_mode;
                // Move (cheaper than copy, and ParsedOutOfLineMemberOwner
                // is copyable but has no reason to pay for a copy here)
                // the owned value out of `owner` -- every path through
                // this `if (match(TokenKind::Tilde))` block returns
                // before `owner` could ever be read again.
                parsed.owner = std::move(owner).value();
                parsed.kind = OutOfLineMemberKind::Destructor;
                parsed.fn.return_type.kind = TypeKind::Named;
                parsed.fn.return_type.name = "void";
                auto lparen_result = expect(TokenKind::LParen, "'('");
                if (!lparen_result.has_value()) return std::unexpected(std::move(lparen_result).error());
                auto rparen_result = expect(TokenKind::RParen, "')'");
                if (!rparen_result.has_value()) return std::unexpected(std::move(rparen_result).error());
                auto body_result = parse_out_of_line_member_body_or_default(parsed.fn, "an out-of-line destructor definition");
                if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
                return finish_out_of_line_member_definition(program, std::move(parsed));
            }
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == owner->unqualified_name &&
                peek_at(1).kind == TokenKind::LParen) {
                advance();
                ParsedOutOfLineMemberDefinition parsed{};

                parsed.fn.loc = prefix.loc;
                parsed.fn.is_unsafe = prefix.is_unsafe;
                parsed.fn.is_nodiscard = prefix.is_nodiscard;
                parsed.fn.nodiscard_reason = prefix.nodiscard_reason;
                parsed.fn.eval_mode = prefix.eval_mode;
                // Move (not copy) -- see the identical destructor-branch
                // comment above; this `if` block also always returns
                // before `owner` could be read again.
                parsed.owner = std::move(owner).value();
                parsed.kind = OutOfLineMemberKind::Constructor;
                parsed.fn.return_type.kind = TypeKind::Named;
                parsed.fn.return_type.name = "void";
                auto params_result = parse_param_list(/*allow_unnamed_single_parameter=*/true);
                if (!params_result.has_value()) return std::unexpected(std::move(params_result).error());
                parsed.fn.params = std::move(params_result).value();
                auto trailing_attrs_result = parse_function_trailing_attributes(parsed.fn, "a constructor declarator");
                if (!trailing_attrs_result.has_value()) return std::unexpected(std::move(trailing_attrs_result).error());
                if (!check(TokenKind::Semicolon) && !check(TokenKind::Assign)) {
                    auto member_inits_result = parse_constructor_member_initializer_list();
                    if (!member_inits_result.has_value()) return std::unexpected(std::move(member_inits_result).error());
                    parsed.fn.member_initializers = std::move(member_inits_result).value();
                }
                auto body_result = parse_out_of_line_member_body_or_default(parsed.fn, "an out-of-line constructor definition");
                if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
                return finish_out_of_line_member_definition(program, std::move(parsed));
            }
            pos_ = saved_pos;
            return false;
        }

        pos_ = after_prefix;
        // Speculative parse: attempt the return-type-first out-of-line
        // member form; on failure, this wasn't that form after all, so
        // backtrack and report "no match" (false), just like the
        // try/catch(const ParseError&) this replaces.
        auto return_type_result = parse_type_with_lifetime_attributes_enabled();
        if (!return_type_result.has_value()) {
            pos_ = saved_pos;
            return false;
        }
        Type return_type = std::move(return_type_result).value();
        std::optional<ParsedOutOfLineMemberOwner> owner = parse_out_of_line_member_owner();
        if (!owner.has_value()) {
            pos_ = saved_pos;
            return false;
        }
        auto colon_result2 = expect(TokenKind::ColonColon, "'::'");
        if (!colon_result2.has_value()) return std::unexpected(std::move(colon_result2).error());

        ParsedOutOfLineMemberDefinition parsed{};

        parsed.fn.loc = prefix.loc;
        parsed.fn.is_unsafe = prefix.is_unsafe;
        parsed.fn.is_nodiscard = prefix.is_nodiscard;
        parsed.fn.nodiscard_reason = prefix.nodiscard_reason;
        parsed.fn.eval_mode = prefix.eval_mode;
        // Move (not copy) -- see the identical comment above; `owner`
        // (declared just above, its own single-use local, already
        // confirmed has_value()) is never read again afterward.
        parsed.owner = std::move(owner).value();
        parsed.fn.return_type = std::move(return_type);

        if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator") {
            advance();
            if (match(TokenKind::Star)) {
                parsed.kind = OutOfLineMemberKind::OperatorDeref;
            } else if (match(TokenKind::Arrow)) {
                parsed.kind = OutOfLineMemberKind::OperatorArrow;
            } else if (match(TokenKind::EqualEqual)) {
                parsed.kind = OutOfLineMemberKind::OperatorEqual;
            } else if (match(TokenKind::NotEqual)) {
                parsed.kind = OutOfLineMemberKind::OperatorNotEqual;
            } else if (match(TokenKind::Assign)) {
                parsed.kind = OutOfLineMemberKind::OperatorAssign;
            } else {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column,
                                 "unsupported out-of-line member operator definition in this version"));
            }
        } else {
            parsed.kind = OutOfLineMemberKind::Method;
            auto method_name_result = expect(TokenKind::Identifier, "method name");
            if (!method_name_result.has_value()) return std::unexpected(std::move(method_name_result).error());
            parsed.member_name = std::string(method_name_result.value().text.data(), method_name_result.value().text.size());
        }

        bool allow_unnamed_single_parameter =
            parsed.kind == OutOfLineMemberKind::OperatorEqual || parsed.kind == OutOfLineMemberKind::OperatorNotEqual ||
            parsed.kind == OutOfLineMemberKind::OperatorAssign;
        auto params_result2 = parse_param_list(allow_unnamed_single_parameter);
        if (!params_result2.has_value()) return std::unexpected(std::move(params_result2).error());
        parsed.fn.params = std::move(params_result2).value();
        auto trailing_attrs_result2 = parse_function_trailing_attributes(parsed.fn, "a member function declarator");
        if (!trailing_attrs_result2.has_value()) return std::unexpected(std::move(trailing_attrs_result2).error());
        parsed.is_const_method = match(TokenKind::KwConst);
        parsed.fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
        auto body_result2 = parse_out_of_line_member_body_or_default(parsed.fn, "an out-of-line member definition");
        if (!body_result2.has_value()) return std::unexpected(std::move(body_result2).error());
        return finish_out_of_line_member_definition(program, std::move(parsed));
    }

    // Multiple identical bodyless `extern "C"` declarations all describe
    // the same global C-linkage symbol, even if one arrived via an
    // imported module's hidden compile-time payload and another was
    // written locally. Keep just one declaration so later overload-table
    // construction doesn't treat that redeclaration as a conflicting
    // overload.
    void reconcile_identical_extern_c_declarations(Program& program) {
        std::vector<Function> reconciled{};

        reconciled.reserve(program.functions.size());
        std::vector<bool> consumed{};
        consumed.resize(program.functions.size(), false);
        for (std::size_t i = 0; i < program.functions.size(); i++) {
            if (consumed[i]) continue;
            Function& source = program.functions[i];
            Function merged = std::move(source);
            consumed[i] = true;
            if (!is_bodyless_extern_c_declaration(merged)) {
                reconciled.push_back(std::move(merged));
                continue;
            }
            for (std::size_t j = i + 1; j < program.functions.size(); j++) {
                Function& candidate = program.functions[j];
                if (consumed[j] || !is_bodyless_extern_c_declaration(candidate) ||
                    !same_function_signature(merged, candidate)) {
                    continue;
                }
                // Saved before the possible std::move(candidate) below --
                // self-hosting treats a moved-from reference's own fields
                // as no longer readable at all (even a plain bool), so
                // the post-merge combination below reads this saved copy
                // instead of re-reading `candidate` directly.
                bool candidate_was_compile_time_dependency = candidate.is_compile_time_dependency;
                if (merged.is_compile_time_dependency && !candidate.is_compile_time_dependency) {
                    Function local = std::move(candidate);
                    local.is_exported = local.is_exported || merged.is_exported;
                    merged = std::move(local);
                } else {
                    merged.is_exported = merged.is_exported || candidate.is_exported;
                }
                merged.is_compile_time_dependency = merged.is_compile_time_dependency && candidate_was_compile_time_dependency;
                consumed[j] = true;
            }
            reconciled.push_back(std::move(merged));
        }
        program.functions = std::move(reconciled);
    }

    [[nodiscard]] std::expected<void, ParseError> reconcile_ordinary_forward_declarations(Program& program) {
        std::vector<Function> reconciled{};

        reconciled.reserve(program.functions.size());
        std::vector<bool> consumed{};
        consumed.resize(program.functions.size(), false);
        for (std::size_t i = 0; i < program.functions.size(); i++) {
            if (consumed[i]) continue;
            // Moved into a plain, owned local value immediately --
            // mirroring reconcile_identical_extern_c_declarations's own
            // `Function& source = program.functions[i]; Function merged
            // = std::move(source);` pattern just above -- rather than
            // kept as a `Function&`/`const Function&` reference into
            // program.functions[i] for the rest of this iteration:
            // `program` is itself a *mutable*-reference parameter, and
            // self-hosting only allows one live reborrow from such a
            // lender at a time (ch05 §5.7's reborrow-suspension rule),
            // regardless of whether the reborrows involved are const --
            // so keeping any named reference here alive would conflict
            // with `candidate`'s own reborrow of program.functions[j]
            // below. Once moved into `fn`, an ordinary owned value, it
            // no longer holds any borrow of `program` at all, so it can
            // coexist with however many later reborrows of
            // program.functions[j] the loops below need, one at a time.
            // (std::move itself requires a plain identifier argument --
            // ch05 §6.2 -- hence the `fn_source` reference as the
            // necessary intermediate, exactly like `source` above.)
            Function& fn_source = program.functions[i];
            Function fn = std::move(fn_source);
            consumed[i] = true;
            if (!is_bodyless_free_function_forward_decl(fn)) {
                reconciled.push_back(std::move(fn));
                continue;
            }
            bool merged = false;
            std::vector<SourceLocation> superseded_locs{};
            superseded_locs.push_back(fn.loc);
            for (std::size_t j = i + 1; j < program.functions.size(); j++) {
                Function& candidate = program.functions[j];
                if (!same_function_signature(fn, candidate)) continue;
                if (is_bodyless_free_function_forward_decl(candidate)) {
                    consumed[j] = true;
                    superseded_locs.push_back(candidate.loc);
                    continue;
                }
                // Moved into its own owned value for the same reason as
                // `fn` above: `candidate` (a reference into
                // program.functions[j]) must stop reborrowing `program`
                // before the *next* j-iteration's own `candidate` binds
                // -- std::move requires the plain-identifier argument
                // `candidate` itself provides here.
                Function merged_fn = std::move(candidate);
                for (std::size_t p = 0; p < fn.params.size() && p < merged_fn.params.size(); p++) {
                    if (merged_fn.params[p].default_expr == nullptr && fn.params[p].default_expr != nullptr) {
                        merged_fn.params[p].default_expr =
                            std::shared_ptr<Expr>(deep_clone_expr(*fn.params[p].default_expr).release());
                    }
                }
                merged_fn.is_exported = merged_fn.is_exported || fn.is_exported;
                merged_fn.superseded_forward_declaration_locs = std::move(superseded_locs);
                reconciled.push_back(std::move(merged_fn));
                consumed[j] = true;
                merged = true;
                break;
            }
            if (!merged) {
                for (std::size_t j = i + 1; j < program.functions.size(); j++) {
                    const Function& candidate = program.functions[j];
                    if (!same_function_declarator(fn, candidate)) continue;
                    // [dcl.fct.def.delete]/4, the free-function twin of
                    // the member rule in
                    // finish_out_of_line_member_definition: the
                    // declarator matches, so the generic mismatch
                    // message below would answer the wrong question.
                    if (candidate.is_deleted && !fn.is_deleted) {
                        std::string _msg_delete4{"'= delete' on a later declaration of '"};
                        _msg_delete4 += fn.name;
                        _msg_delete4 += "': a deleted definition shall be the first declaration of the function ";
                        _msg_delete4 += "([dcl.fct.def.delete]/4) -- write '= delete' on the first declaration instead";
                        return std::unexpected(ParseError(candidate.loc.line, candidate.loc.column, _msg_delete4));
                    }
                    {
                        std::string _msg_2759{"definition of ordinary forward declaration '"};
                        _msg_2759 += fn.name;
                        _msg_2759 += "' does not match its earlier declaration exactly";
                        return std::unexpected(ParseError(candidate.loc.line, candidate.loc.column,
                                     _msg_2759));
                    }
                }
            }
            if (!merged) {
                {
                    std::string _msg_2765{"ordinary forward declaration of function '"};
                    _msg_2765 += fn.name;
                    _msg_2765 += "' must be followed by a matching definition in the same translation unit";
                    return std::unexpected(ParseError(fn.loc.line, fn.loc.column,
                                 _msg_2765));
                }
            }
        }
        for (std::size_t i = 0; i < program.functions.size(); i++) {
            if (consumed[i]) continue;
            Function& remaining = program.functions[i];
            reconciled.push_back(std::move(remaining));
        }
        program.functions = std::move(reconciled);
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> ensure_member_forward_declarations_are_defined(const Program& program) {
        for (const Function& fn : program.functions) {
            if (!is_bodyless_member_forward_decl(fn)) continue;
            std::string display_name{fn.member_owner_class};
            display_name += "::";
            display_name += fn.name.substr(fn.name.rfind('_') + 1);
            if (is_special_member_mangled_name(fn.name, fn.member_owner_class, "_new")) {
                std::size_t pos = fn.member_owner_class.rfind("::");
                std::string unqualified{};
                if (pos == static_cast<std::size_t>(-1)) {
                    unqualified = fn.member_owner_class;
                } else {
                    unqualified = fn.member_owner_class.substr(pos + 2);
                }
                display_name = fn.member_owner_class;
                display_name += "::";
                display_name += unqualified;
            } else if (is_special_member_mangled_name(fn.name, fn.member_owner_class, "_delete")) {
                std::size_t pos = fn.member_owner_class.rfind("::");
                std::string unqualified{};
                if (pos == static_cast<std::size_t>(-1)) {
                    unqualified = fn.member_owner_class;
                } else {
                    unqualified = fn.member_owner_class.substr(pos + 2);
                }
                display_name = fn.member_owner_class;
                display_name += "::~";
                display_name += unqualified;
            } else if (fn.name.ends_with("_operator_assign")) {
                display_name = fn.member_owner_class;
                display_name += "::operator=";
            } else if (fn.name.ends_with("_operator_equal")) {
                display_name = fn.member_owner_class;
                display_name += "::operator==";
            } else if (fn.name.ends_with("_operator_not_equal")) {
                display_name = fn.member_owner_class;
                display_name += "::operator!=";
            } else if (fn.name.ends_with("_operator_deref")) {
                display_name = fn.member_owner_class;
                display_name += "::operator*";
            } else if (fn.name.ends_with("_operator_arrow")) {
                display_name = fn.member_owner_class;
                display_name += "::operator->";
            }
            {
                std::string _msg_2802{"member declaration '"};
                _msg_2802 += display_name;
                _msg_2802 += "' must be followed by a matching out-of-line definition in the same translation unit";
                return std::unexpected(ParseError(fn.loc.line, fn.loc.column,
                             _msg_2802));
            }
        }
        return {};
    }

    [[nodiscard]] static bool is_shadowed_local(
        const std::string& name, const std::vector<std::unordered_set<std::string>>& scopes) {
        // std::vector has no rbegin()/rend() yet -- walk backwards by index.
        for (std::size_t i = scopes.size(); i > 0; i--) {
            if (scopes[i - 1].contains(name)) return true;
        }
        return false;
    }

    void qualify_same_namespace_function_calls(Program& program) {
        std::unordered_set<std::string> known_function_names{};

        for (const Function& fn : program.functions) known_function_names.insert(fn.name);
        // `TypeName{args}`/`TypeName{}` used as a value (not a VarDecl
        // initializer) parses as an ordinary Call node whose name is the
        // bare type name (see parse_return's Identifier+LBrace rewrite,
        // and ExprKind::ValueInit's own comment) -- exactly as call-able,
        // from a same-namespace context, as a sibling free function is.
        // Class/struct names are namespace-qualified at declaration parse
        // time just like function names are (see qualify_name's callers),
        // so a same-namespace `TypeName{args}` call site needs the exact
        // same reconciliation below, or it can never resolve against the
        // qualified name codegen actually registered it under.
        for (const ClassDef& def : program.classes) known_function_names.insert(def.name);
        for (const StructDef& def : program.structs) known_function_names.insert(def.name);
        for (Function& fn : program.functions) {
            if (fn.body == nullptr || fn.namespace_path.empty()) continue;
            std::vector<std::unordered_set<std::string>> scopes{};
            scopes.emplace_back();
            for (const Param& param : fn.params) scopes.back().insert(param.name);
            std::string namespace_prefix = join_namespace_path(fn.namespace_path);
            // A constructor's member-initializer-list (`: field{expr}, ...`)
            // is stored separately from fn.body (see MemberInitializer/
            // Initializer, ast.cppm) -- walked here explicitly too, since
            // it's just as capable of containing an unqualified call to a
            // sibling same-namespace free function (e.g. `loc{make_source_
            // location(line, column)}`) as any ordinary body statement is,
            // but is otherwise never reached by the qualify_same_namespace_
            // function_calls_in_stmt walk below (which only ever recurses
            // through *fn.body).
            for (MemberInitializer& init : fn.member_initializers) {
                if (init.initializer.expr != nullptr) {
                    qualify_same_namespace_function_calls_in_expr(*init.initializer.expr, namespace_prefix,
                                                                  known_function_names, scopes);
                }
                for (ExprPtr& arg : init.initializer.brace_args) {
                    qualify_same_namespace_function_calls_in_expr(*arg, namespace_prefix, known_function_names, scopes);
                }
            }
            qualify_same_namespace_function_calls_in_stmt(*fn.body, namespace_prefix, known_function_names, scopes);
        }
    }

    void qualify_same_namespace_function_calls_in_stmt(
        Stmt& stmt, const std::string& namespace_prefix, const std::unordered_set<std::string>& known_function_names,
        std::vector<std::unordered_set<std::string>>& scopes) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init != nullptr) {
                    qualify_same_namespace_function_calls_in_expr(*stmt.init, namespace_prefix, known_function_names,
                                                                  scopes);
                }
                for (ExprPtr& arg : stmt.ctor_args) {
                    qualify_same_namespace_function_calls_in_expr(*arg, namespace_prefix, known_function_names, scopes);
                }
                scopes.back().insert(stmt.var_name);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr != nullptr) {
                    qualify_same_namespace_function_calls_in_expr(*stmt.expr, namespace_prefix, known_function_names,
                                                                  scopes);
                }
                return;
            case StmtKind::If:
                qualify_same_namespace_function_calls_in_expr(*stmt.condition, namespace_prefix, known_function_names,
                                                              scopes);
                scopes.emplace_back();
                qualify_same_namespace_function_calls_in_stmt(*stmt.then_branch, namespace_prefix, known_function_names,
                                                              scopes);
                scopes.pop_back();
                if (stmt.else_branch != nullptr) {
                    scopes.emplace_back();
                    qualify_same_namespace_function_calls_in_stmt(*stmt.else_branch, namespace_prefix,
                                                                  known_function_names, scopes);
                    scopes.pop_back();
                }
                return;
            case StmtKind::While:
                qualify_same_namespace_function_calls_in_expr(*stmt.condition, namespace_prefix, known_function_names,
                                                              scopes);
                scopes.emplace_back();
                qualify_same_namespace_function_calls_in_stmt(*stmt.then_branch, namespace_prefix, known_function_names,
                                                              scopes);
                scopes.pop_back();
                return;
            case StmtKind::Switch:
                qualify_same_namespace_function_calls_in_expr(*stmt.condition, namespace_prefix, known_function_names,
                                                              scopes);
                scopes.emplace_back();
                for (SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value != nullptr) {
                        qualify_same_namespace_function_calls_in_expr(*switch_case.value, namespace_prefix,
                                                                      known_function_names, scopes);
                    }
                    for (StmtPtr& child : switch_case.statements) {
                        qualify_same_namespace_function_calls_in_stmt(*child, namespace_prefix, known_function_names,
                                                                      scopes);
                    }
                }
                scopes.pop_back();
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough: return;
            case StmtKind::Block:
                scopes.emplace_back();
                for (StmtPtr& child : stmt.statements) {
                    qualify_same_namespace_function_calls_in_stmt(*child, namespace_prefix, known_function_names, scopes);
                }
                scopes.pop_back();
                return;
        }
    }

    void qualify_same_namespace_function_calls_in_expr(
        Expr& expr, const std::string& namespace_prefix, const std::unordered_set<std::string>& known_function_names,
        std::vector<std::unordered_set<std::string>>& scopes) {
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr && !expr.name.empty() &&
            !expr.explicit_global_qualification && expr.name.find("::") == static_cast<std::size_t>(-1) &&
            !is_shadowed_local(expr.name, scopes)) {
            std::string candidate{namespace_prefix};
            candidate += "::";
            candidate += expr.name;
            if (known_function_names.contains(candidate)) expr.name = std::move(candidate);
        }
        if (expr.lhs != nullptr) {
            qualify_same_namespace_function_calls_in_expr(*expr.lhs, namespace_prefix, known_function_names, scopes);
        }
        if (expr.rhs != nullptr) {
            qualify_same_namespace_function_calls_in_expr(*expr.rhs, namespace_prefix, known_function_names, scopes);
        }
        if (expr.third != nullptr) {
            qualify_same_namespace_function_calls_in_expr(*expr.third, namespace_prefix, known_function_names, scopes);
        }
        for (ExprPtr& arg : expr.args) {
            qualify_same_namespace_function_calls_in_expr(*arg, namespace_prefix, known_function_names, scopes);
        }
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init != nullptr) {
                qualify_same_namespace_function_calls_in_expr(*capture.init, namespace_prefix, known_function_names,
                                                              scopes);
            }
        }
        if (expr.lambda_body != nullptr) {
            scopes.emplace_back();
            for (const Param& param : expr.lambda_params) scopes.back().insert(param.name);
            // Named distinctly from the mutable `capture` loop variable
            // above purely for readability -- the two are different
            // declarations in sibling scopes, and movecheck keys each
            // local by its own declaration (mir.cppm's LocalId), so
            // reusing the spelling would be correct too.
            for (const LambdaCapture& scope_capture : expr.lambda_captures) scopes.back().insert(scope_capture.name);
            qualify_same_namespace_function_calls_in_stmt(*expr.lambda_body, namespace_prefix, known_function_names,
                                                          scopes);
            scopes.pop_back();
        }
    }

    // Splits a dotted module name ("org.lotx.cmath") into its segments
    // ({"org", "lotx", "cmath"}), so it can be compared segment-for-
    // segment against a `::`-based namespace_path (ch11 §11.5: module
    // names use '.', namespace paths use '::' -- translated one to the
    // other segment-for-segment, never string-compared directly).
    [[nodiscard]] static std::vector<std::string> split_dotted_name(const std::string& dotted) {
        std::vector<std::string> segments{};

        std::size_t start = 0;
        while (start <= dotted.size()) {
            std::size_t dot = dotted.find('.', start);
            if (dot == static_cast<std::size_t>(-1)) {
                segments.push_back(dotted.substr(start));
                break;
            }
            segments.push_back(dotted.substr(start, dot - start));
            start = dot + 1;
        }
        return segments;
    }

    // `export` is only meaningful inside a module unit; once we're in a
    // module, real C++ places no namespace-shape restriction on the
    // exported declaration itself, so neither do we.
    [[nodiscard]] std::expected<void, ParseError> check_export_context(const Program& program, bool is_exported,
                                 const std::vector<std::string>& namespace_path [[maybe_unused]], SourceLocation loc,
                                 const std::string& what) const {
        if (!is_exported) return {};
        if (program.module_name.empty()) {
            {
                std::string _msg_2971{"'export' on "};
                _msg_2971 += what;
                _msg_2971 += " has no effect: this file has no 'export module'/'module' declaration ";
                _msg_2971 += "(ch11 §11.3)";
                return std::unexpected(ParseError(loc.line, loc.column,
                              _msg_2971));
            }
        }
        return {};
    }

    // [dcl.ptr]/1 + [dcl.type.cv]/1: applies a written `const` to the base
    // type and then consumes the declarator's `*` levels, each of which
    // may itself be followed by its own `const` (`int* const p` -- a const
    // *pointer* to a mutable int, as distinct from `const int* p`).
    //
    // `leading_const` is the west-const spelling parse_type() already
    // consumed before this type; a trailing `const` accepted right here is
    // the east-const spelling of the very same thing (`int const` ==
    // `const int`, [dcl.type.cv]/1 -- the two orders are one type, not
    // two). Both land on the *base* type's own `is_const_qualified`, so
    // there is one representation for one qualifier rather than a `bool`
    // travelling beside the type.
    //
    // Every pointer level is built by ast.cppm's `pointer_to`, which owns
    // the single canonical spelling of `const T*` (qualifier on the
    // pointer's `is_mutable_pointee`, never left on the pointee) -- this
    // used to be open-coded twice in this function with a
    // `first_star && const_qualifies_first_pointer` guard, which is the
    // same reading, arrived at separately. `const int**` still means
    // "pointer to (pointer to const int)": the qualifier is consumed into
    // the innermost level and the outer `pointer_to` sees an already-
    // unqualified pointee.
    [[nodiscard]] std::expected<Type, ParseError> parse_pointer_suffixes(Type type, bool leading_const) {
        if (leading_const) type.is_const_qualified = true;
        if (match(TokenKind::KwConst)) type.is_const_qualified = true;
        while (match(TokenKind::Star)) {
            type = pointer_to(std::move(type));
            if (match(TokenKind::KwConst)) type.is_const_qualified = true;
        }
        return type;
    }

    // Parses a base type name (`int`, `bool`, `std::unique_ptr<T>`, or a
    // known struct name) followed by zero or more `*` for pointer levels.
    // Array suffixes (`[N]`) are handled separately by parse_array_suffix,
    // since in C-style declarators the array size follows the *declared
    // name*, not the type. `const_qualifies_first_pointer` is set by
    // parse_type() when it saw a leading `const` immediately before this
    // call; see parse_pointer_suffixes for how it (and the east-const
    // spelling) are applied.
    [[nodiscard]] std::expected<Type, ParseError> parse_unqualified_type(bool const_qualifies_first_pointer = false) {
        if (std::optional<std::string> std_builtin_scalar = peek_std_qualified_builtin_scalar_type_name();
            std_builtin_scalar.has_value()) {
            Type type{};

            type.kind = TypeKind::Named;
            type.name = *std_builtin_scalar;
            consume_std_qualified();
            bool first_star = true;
            while (match(TokenKind::Star)) {
                auto pointee = std::make_shared<Type>(type);
                Type pointer_type{};
                pointer_type.kind = TypeKind::Pointer;
                pointer_type.pointee = std::move(pointee);
                pointer_type.is_mutable_pointee = !(first_star && const_qualifies_first_pointer);
                type = pointer_type;
                first_star = false;
            }
            return type;
        }
        if (check_std_qualified("span")) {
            consume_std_qualified();
            auto lt_result = expect(TokenKind::Less, "'<'");
            if (!lt_result.has_value()) return std::unexpected(std::move(lt_result).error());
            // `const` here qualifies the *element* type (`std::span<const
            // T>`, a read-only view), not a reference -- so it's parsed
            // directly rather than through parse_type() (which only
            // accepts a leading `const` when followed by `&`).
            bool element_is_const = match(TokenKind::KwConst);
            auto element_result = parse_unqualified_type();
            if (!element_result.has_value()) return std::unexpected(std::move(element_result).error());
            Type element = std::move(element_result).value();
            auto gt_result = expect(TokenKind::Greater, "'>'");
            if (!gt_result.has_value()) return std::unexpected(std::move(gt_result).error());
            Type type{};

            type.kind = TypeKind::Span;
            type.pointee = std::make_shared<Type>(std::move(element));
            type.is_mutable_ref = !element_is_const;
            return type;
        }

        const Token& tok = peek();
        Type type{};

        type.kind = TypeKind::Named;
        if (std::string_view builtin_name = builtin_scalar_keyword_type_name(tok.kind); !builtin_name.empty() &&
            tok.kind != TokenKind::KwLong) {
            type.name = std::string(builtin_name.data(), builtin_name.size());
            advance();
        } else if (tok.kind == TokenKind::KwNullptrT) {
            // ch06 §6: `nullptr_t` -- `nullptr`'s own type. Deliberately
            // *not* part of builtin_scalar_keyword_type_name's set: that
            // set is exactly the scalar/numeric family a
            // `static_cast<T>` may convert freely between, and
            // `nullptr_t` must never convert to or from an integer.
            type.name = nullptr_type_name();
            advance();
        } else if (tok.kind == TokenKind::KwLong) {
            // ch06 §6: `long` -- deliberately fixed as an alias for
            // int64_t regardless of target platform (unlike real C++'s
            // own platform-defined width), to design away the classic
            // LP64-vs-LLP64 cross-platform pitfall.
            type.name = "long";
            advance();
        } else if (tok.kind == TokenKind::KwUnsigned) {
            // ch06 §6: `unsigned` is only ever legal directly before
            // `int`/`long` -- the bare one-word shorthand (meaning
            // `unsigned int` in real C++) is *not* valid scpp, to keep
            // `unsigned`-anything unambiguous and grep-able.
            advance();
            if (match(TokenKind::KwInt)) {
                type.name = "unsigned int";
            } else if (match(TokenKind::KwLong)) {
                type.name = "unsigned long";
            } else {
                const Token& next = peek();
                {
                    std::string _msg_3059{"'unsigned' must be immediately followed by 'int' or 'long' (ch06 §6) -- the "};
                    _msg_3059 += "bare 'unsigned' shorthand is not valid scpp";
                    return std::unexpected(ParseError(next.line, next.column,
                                  _msg_3059));
                }
            }
        } else if (tok.kind == TokenKind::Identifier &&
                   peek_at(1).kind != TokenKind::Less &&
                   find_injected_generic_type_name(std::string(tok.text.data(), tok.text.size())) != nullptr) {
            // find_injected_generic_type_name is a raw InjectedGenericTypeName*
            // known non-null here (the `!= nullptr` check just above), but
            // self-hosting still requires an explicit `[[scpp::unsafe]] { }`
            // to dereference any raw pointer (ch01 §1.3/ch02); `injected`
            // itself doesn't escape this branch, so the whole branch body
            // fits inside the one block.
            [[scpp::unsafe]] {
                const InjectedGenericTypeName& injected = *find_injected_generic_type_name(std::string(tok.text.data(), tok.text.size()));
                type.name = injected.qualified_name;
                for (const GenericTypeParam& param : injected.template_params) {
                    if (param.is_non_type) {
                        type.non_type_args.push_back(std::shared_ptr<Expr>(make_identifier_expr(current_loc(), param.name).release()));
                        continue;
                    }
                    Type arg{};

                    arg.kind = TypeKind::Named;
                    arg.name = param.name;
                    arg.is_pack_expansion = param.is_pack;
                    type.template_args.push_back(std::move(arg));
                }
            }
            advance();
        } else if (tok.kind == TokenKind::Identifier &&
                   generic_type_names_.contains(resolve_visible_type_name(peek_qualified_name()))) {
            // ch05 §5.14: `Name<Arg, Arg2, ...>` -- a generic class/
            // struct instantiation. `name` still names the *template*
            // here, not a real, concrete type -- left for the
            // Monomorphizer to resolve (synthesizing the concrete
            // instantiation and rewriting `name` to its own mangled
            // name) exactly like a Lambda literal's own synthesized
            // class or an `auto` VarDecl's inferred type. An ordinary
            // (non-variadic) generic type takes exactly one type
            // argument (GenericTypeParam's own single-parameter scope,
            // enforced right here since resolve_generic_type's own
            // "template_args empty means not a generic instantiation at
            // all" fast path depends on an ordinary generic never
            // parsing with zero); a variadic one (Tuple/TupleImpl-style,
            // tracked in variadic_primary_template_params_) takes its
            // own primary template's own leading non-type arguments (if
            // any, e.g. TupleImpl's own "Idx" position -- parsed as an
            // expression, non_type_args, ch05 §5.14's bit-pattern-
            // equality-matched non-type parameters) followed by zero or
            // more comma-separated type arguments (one per pack
            // element, e.g. `Tuple<int, bool, char>`). Inside a generic
            // *function*'s own base-class-deduction parameter type
            // (`TupleImpl<I, Head, Tail...>& t`, ch05 §5.14's `get<I>`
            // pattern), each argument may instead *symbolically*
            // reference the enclosing function template's own parameter
            // names directly (a non-type parameter's name as a bare
            // expression, e.g. "I"; a type parameter's own name, e.g.
            // "Head", parsing as an ordinary Named type since it's
            // already been temporarily registered exactly like a class/
            // struct template's own type parameter is -- see
            // parse_generic_function_def); the *one* new syntax this
            // parser needs to recognize structurally is a trailing pack
            // spread, `Name...` (must be the final argument), handled
            // right here regardless of context since `...` is never a
            // valid continuation of an ordinary type otherwise.
            const Token& name_tok = peek();
            type.name = resolve_visible_type_name(parse_qualified_name());
            bool is_variadic = variadic_primary_template_params_.contains(type.name);
            // ch05 §5.14: `unordered_map` has no `operator[]`, and its
            // `.find()` returns a raw pointer (self-hosting; requires
            // `[[scpp::unsafe]] { }` to dereference) -- since this
            // ordinary-params vector is consulted at several scattered
            // points below, look it up fresh via `.at()` (already
            // internally unsafe-wrapped, so it hands back a plain safe
            // reference) each time, guarded by this one `.contains()`
            // check, rather than keeping a raw pointer alive across
            // statements.
            bool has_ordinary_params = ordinary_generic_type_template_params_.contains(type.name);
            std::size_t leading_non_type_count = 0;
            if (is_variadic) {
                // ch05 §5.14: `unordered_map` has no `operator[]` (self-
                // hosting; std_unordered_map.scpp's own comment) -- use
                // `.at()`, safe here since `is_variadic` (just above)
                // already confirmed this key is present. Bound to an
                // explicit named reference first (rather than iterated
                // directly), matching
                // try_parse_explicit_generic_type_constructor_template_args's
                // own identical precedent below: a range-for directly
                // over a `this`-derived `.at()` call would otherwise
                // resolve *two* separate reborrows back to the same
                // `this` lender (one for the implicit range access, one
                // per element), tripping movecheck's single-outstanding-
                // reborrow rule even though both are read-only here.
                const std::vector<GenericTypeParam>& variadic_params = variadic_primary_template_params_.at(type.name);
                for (const GenericTypeParam& p : variadic_params) {
                    if (!p.is_non_type) break;
                    leading_non_type_count++;
                }
            }
            if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            std::size_t arg_index = 0;
            if (!check(TokenKind::Greater)) {
                while (true) {
                    bool parse_non_type_arg =
                        is_variadic ? arg_index < leading_non_type_count
                                    : (has_ordinary_params &&
                                       arg_index < ordinary_generic_type_template_params_.at(type.name).size() &&
                                       ordinary_generic_type_template_params_.at(type.name)[arg_index].is_non_type);
                    if (parse_non_type_arg) {
                        auto non_type_arg_result = parse_additive();
                        if (!non_type_arg_result.has_value()) return std::unexpected(std::move(non_type_arg_result).error());
                        type.non_type_args.push_back(std::shared_ptr<Expr>(std::move(non_type_arg_result).value().release()));
                    } else if (is_variadic && check(TokenKind::Identifier) && peek_at(1).kind == TokenKind::Ellipsis) {
                        Type spread{};

                        spread.kind = TypeKind::Named;
                        const Token& spread_name_tok = advance();
                        spread.name = std::string(spread_name_tok.text.data(), spread_name_tok.text.size());
                        advance(); // '...'
                        spread.is_pack_expansion = true;
                        type.template_args.push_back(std::move(spread));
                    } else {
                        auto template_arg_result = parse_template_type_argument();
                        if (!template_arg_result.has_value()) return std::unexpected(std::move(template_arg_result).error());
                        Type __template_arg_result_value = std::move(template_arg_result).value();
                        type.template_args.push_back(std::move(__template_arg_result_value));
                    }
                    arg_index++;
                    if (!(match(TokenKind::Comma))) break;
                }
            }
            if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            if (!is_variadic && has_ordinary_params) {
                std::size_t expected_non_type_args = 0;
                // Bound to an explicit named reference first -- see the
                // range-for/reborrow comment in the is_variadic branch
                // above (same underlying reason, different map).
                const std::vector<GenericTypeParam>& ordinary_params = ordinary_generic_type_template_params_.at(type.name);
                for (const GenericTypeParam& p : ordinary_params) {
                    if (p.is_non_type) expected_non_type_args++;
                }
                std::size_t expected_type_args = ordinary_params.size() - expected_non_type_args;
                if (type.template_args.size() != expected_type_args || type.non_type_args.size() != expected_non_type_args) {
                    {
                        std::string _msg_3171{"'"};
                        _msg_3171 += type.name;
                        _msg_3171 += "' takes exactly ";
                        _msg_3171 += std::to_string(ordinary_generic_type_template_params_.at(type.name).size());
                        _msg_3171 += " template argument(s) in this order (ch05 §5.14)";
                        return std::unexpected(ParseError(name_tok.line, name_tok.column,
                                      _msg_3171));
                    }
                }
            }
           maybe_mark_reference_wrapper_lifetime_source(type);
        } else if (tok.kind == TokenKind::Identifier) {
            std::string spelled_name = peek_qualified_name();
            if (std::optional<Type> alias = resolve_visible_type_alias(spelled_name); alias.has_value()) {
                parse_qualified_name();
                // Move (not copy) the owned Type out of `alias` -- `Type`
                // has no copy-assignment yet (ch04 §4.2); mirrors
                // resolve_visible_type_name's identical
                // `std::move(local).value()` idiom just above.
                type = std::move(alias).value();
            } else if (is_visible_type_name(spelled_name)) {
                type.name = resolve_visible_type_name(parse_qualified_name());
            } else {
                return std::unexpected(ParseError(tok.line, tok.column, "expected a type name"));
            }
        } else {
            return std::unexpected(ParseError(tok.line, tok.column, "expected a type name"));
        }

        return parse_pointer_suffixes(std::move(type), const_qualifies_first_pointer);
    }

    // Parses a full type, including the borrow-checking sugar from ch03:
    // an optional leading `const` plus a trailing `&` turns the
    // unqualified type into a Reference -- `T&` is a mutable/exclusive
    // borrow, `const T&` a shared borrow (ch05.2). A trailing `&&`
    // instead makes an rvalue reference (`T&&`, ch03's "passed by move"
    // parameter form) -- ownership transfer, not a borrow; `const T&&`
    // is rejected (a moved-from value must be mutable to move *from*).
    // `allow_rvalue_ref` gates `&&` to exactly the contexts ch03's own
    // table restricts it to -- a function/method/constructor parameter's
    // declared type -- rejected everywhere else (a var-decl, struct/
    // class field, return type, or a nested position like std::
    // unique_ptr<T>/std::make_unique<T>'s own `T`) with a clear parse
    // error, rather than silently constructing an AST some later pass
    // would have to reject (or worse, wouldn't). `const` immediately
    // before a *pointer* type (`const T*`, e.g. `const char* fmt` in a
    // realistic `extern "C"` signature -- ch02 §2.1) is also accepted
    // and, like a reference's `is_mutable_ref`, properly tracked: `const
    // T*` and `T*` are genuinely distinct types (ch05 §5.7, ch08 Q9),
    // not unified the way an earlier draft of that section assumed.
    // A bare `const T` (no `&`/`&&`/`*` at all) is a `const`-qualified
    // value type, accepted in every position a type may be written:
    // [dcl.type.cv]/1 puts the qualifier on the type, and the spec adopts
    // no clause narrowing where it may appear, so a parameter, data
    // member, return type or template argument may all carry it exactly
    // as in ISO C++. `out_bare_const` (non-null) additionally *reports*
    // the qualifier to parse_var_decl, which still records it on
    // Stmt::is_const for the deduced-`auto` case, where the type this
    // function returns is only the `auto` placeholder.
    //
    // This used to be three mutually exclusive answers to one question:
    // `out_bare_const` non-null returned an *unqualified* type and set
    // the flag, `allow_const_qualified_value_type` set the qualifier, and
    // every other caller got a parse error. #492 fixed the first arm by
    // also setting the qualifier; the third arm is why `int f(const int
    // v)`, `const int f()` and `struct S { const int x; }` -- all plain,
    // unremarkable C++ -- were parse errors, and why a half-represented
    // qualifier could disagree with itself depending on which position it
    // was written in.
    [[nodiscard]] std::expected<Type, ParseError> parse_type(bool allow_rvalue_ref = false, bool* out_bare_const = nullptr) {
        // Types are checked on the finished type rather than by counting
        // recursion, because the parser builds pointer and array types
        // with loops -- `int` followed by two thousand stars costs the
        // parser no depth at all and still produces a two-thousand-link
        // chain for every later pass to recurse over.
        auto type_result = parse_type_inner(allow_rvalue_ref, out_bare_const);
        if (type_result.has_value() && type_nesting_exceeds(type_result.value(), kMaxNestingDepth)) {
            return std::unexpected(nesting_too_deep_error("type"));
        }
        return type_result;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_type_inner(bool allow_rvalue_ref, bool* out_bare_const) {
        bool has_const_prefix = match(TokenKind::KwConst);
        auto type_result = parse_unqualified_type(/*const_qualifies_first_pointer=*/has_const_prefix);
        if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
        Type type = std::move(type_result).value();

        if (match(TokenKind::Amp)) {
            // A reference records its referent's constness in
            // `is_mutable_ref`, never as a qualifier on the pointee --
            // the same single-spelling invariant `pointer_to` keeps for
            // pointers, and for the same reason (types_equal compares
            // both, so two spellings of `const T&` would not compare
            // equal). Applies to the east spelling too: `int const& r`
            // arrives here with the qualifier already on `type`.
            bool referent_is_const = has_const_prefix || type.is_const_qualified;
            type.is_const_qualified = false;
            auto pointee = std::make_shared<Type>(std::move(type));
            Type reference_type{};
            reference_type.kind = TypeKind::Reference;
            reference_type.pointee = std::move(pointee);
            reference_type.is_mutable_ref = !referent_is_const;
            return reference_type;
        }

        if (match(TokenKind::AmpAmp)) {
            if (!allow_rvalue_ref) {
                const Token& tok = peek();
                {
                    std::string _msg_3250{"'&&' (rvalue reference) is only supported for a function/method/"};
                    _msg_3250 += "constructor parameter's declared type in this version (ch03) -- not ";
                    _msg_3250 += "a variable, field, return type, or nested type argument";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_3250));
                }
            }
            if (has_const_prefix || type.is_const_qualified) {
                const Token& tok = peek();
                {
                    std::string _msg_3257{"'const' cannot qualify an rvalue reference ('const T&&') -- an "};
                    _msg_3257 += "rvalue-reference parameter always takes ownership via move (ch03), ";
                    _msg_3257 += "which needs mutable access to the value being moved from";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_3257));
                }
            }
            auto pointee = std::make_shared<Type>(std::move(type));
            Type reference_type{};
            reference_type.kind = TypeKind::Reference;
            reference_type.pointee = std::move(pointee);
            reference_type.is_mutable_ref = true;
            reference_type.is_rvalue_ref = true;
            return reference_type;
        }

        // [dcl.type.cv]/1: the qualifier belongs on the type, in every
        // position a type may be written. `parse_pointer_suffixes` has
        // already applied it (from either the west `const T` spelling
        // whose token `has_const_prefix` records, or the east `T const`
        // one) -- so all that is left here is to *report* it through
        // `out_bare_const` for parse_var_decl, whose deduced-`auto`
        // path has no concrete type to carry it on yet.
        //
        // Nothing is rejected: the previous three-way split (report and
        // return unqualified / qualify / parse error) was one question
        // answered three ways depending on which caller asked, which is
        // how `int f(const int v)` came to be a parse error while `const
        // int v = ...;` was not. The `allow_const_qualified_value_type`
        // opt-in that arm needed is gone with it, and so is the
        // `type.kind != TypeKind::Pointer` guard: `const T*` already carries
        // its qualifier as `is_mutable_pointee`, so `is_const_qualified`
        // is unset on it and there is nothing left to report.
        if (out_bare_const != nullptr && type.is_const_qualified) {
            // out_bare_const is a raw bool* known non-null here (the
            // `!= nullptr` check just above); self-hosting still
            // requires an explicit `[[scpp::unsafe]] { }` to
            // dereference any raw pointer (ch01 §1.3/ch02).
            [[scpp::unsafe]] {
                    *out_bare_const = true;
            }
        }
        return type;
    }

    // ch05 §5.11: parses a single *parameter's* declared type, additionally
    // recognizing the abbreviated generic-function form -- `[const]
    // ConceptName auto[&|&&]` (e.g. `const Shape auto&`, `Invocable
    // auto&&`, bare `Shape auto`), *and* the unconstrained `[const]
    // auto[&|&&]` form (no concept name at all, e.g. plain `auto x`) --
    // on top of every ordinary shape parse_type() already handles
    // (including `T&&`, always legal in parameter position). Sets
    // `out_generic_concept` to the concept's own name when this
    // parameter is generic-constrained (left empty otherwise, the
    // overwhelmingly common case) -- see Param::generic_concept's own
    // comment for how this is used later (concept-satisfaction checking
    // + monomorphization). A bare `auto` (no concept) sets it to the
    // reserved "$auto" witness (parse_program's own comment) -- ch05
    // §5.11: "the parameter's type is treated as fully opaque... exactly
    // as if it were constrained by a concept whose requires-expression
    // guarantees nothing".
    //
    // The resulting Type's innermost Named type names the concept's own
    // witness class (ClassDef::is_concept_witness) -- registered in
    // struct_names_/class_names_ exactly like a real class, by
    // parse_concept_def -- so the generic function's own body-check
    // resolves every call through its constrained parameter via the
    // exact same class/method-call machinery used for a real class,
    // with zero new logic; only monomorphization (consulting
    // out_generic_concept, recorded on Param) needs to know this
    // parameter was ever generic at all.
    // [dcl.fct]/5: "any top-level cv-qualifiers modifying a parameter type
    // are deleted when forming the function type". The single place a
    // parameter's declared type is committed, so that the qualifier is
    // guaranteed gone from `Param::type` -- i.e. from the function's
    // *type* -- everywhere it is consumed (overload resolution,
    // redeclaration matching, mangling, `.scppm` payloads), rather than
    // each of those consumers having to remember to ignore it.
    //
    // The parameter object itself is still `const` inside the body; that
    // is what `Param::is_const` records, and build_mir puts it on the
    // parameter's LocalDecl exactly as a `const` local's is recorded.
    // Only the *top* level is deleted -- `const int*` (pointer to const)
    // and `const int&` keep their qualifier, which is not a top-level one.
    void set_param_declared_type(Param& param, Type type) {
        if (type.kind != TypeKind::Reference && type.is_const_qualified) {
            param.is_const = true;
            type.is_const_qualified = false;
        }
        param.type = std::move(type);
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_param_type(std::string& out_generic_concept) {
        out_generic_concept.clear();
        std::size_t const_offset = static_cast<std::size_t>(check(TokenKind::KwConst) ? 1 : 0);
        bool next_is_identifier_then_auto =
            peek_at(const_offset).kind == TokenKind::Identifier && peek_at(const_offset + 1).kind == TokenKind::KwAuto;
        bool next_is_bare_auto = peek_at(const_offset).kind == TokenKind::KwAuto;
        if (!next_is_identifier_then_auto && !next_is_bare_auto) return parse_type(/*allow_rvalue_ref=*/true);

        std::string concept_name{};

        if (next_is_identifier_then_auto) {
            concept_name = std::string(peek_at(const_offset).text.data(), peek_at(const_offset).text.size());
            if (!concept_names_.contains(concept_name)) {
                const Token& tok = peek_at(const_offset);
                {
                    std::string _msg_3328{"'"};
                    _msg_3328 += concept_name;
                    _msg_3328 += "' is not a declared concept -- 'Name auto' is only legal when 'Name' names a ";
                    _msg_3328 += "concept, declared before use (ch05 §5.11)";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_3328));
                }
            }
        } else {
            concept_name = std::string("$auto");
            bare_auto_used_ = true;
        }
        // Only for error messages below -- shows the source spelling
        // ("auto") rather than the internal "$auto" witness-class name a
        // bare parameter is recorded under.
        std::string display_name{};
        if (next_is_identifier_then_auto) {
            display_name = concept_name;
        } else {
            display_name = std::string("auto");
        }
        bool has_const = match(TokenKind::KwConst);
        if (next_is_identifier_then_auto) advance(); // the concept name itself
        if (auto _r = expect(TokenKind::KwAuto, "'auto'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        // East spelling: `auto const v` / `Concept auto const& v` means
        // exactly what the west one does ([dcl.type.cv] places no
        // ordering requirement on a decl-specifier-seq).
        if (match(TokenKind::KwConst)) has_const = true;
        out_generic_concept = concept_name;

        Type type{};

        type.kind = TypeKind::Named;
        type.name = concept_name; // the witness class shares the concept's own name

        if (match(TokenKind::AmpAmp)) {
            if (has_const) {
                const Token& tok = peek();
                {
                    std::string _msg_3354{"'const' cannot qualify an rvalue reference ('const "};
                    _msg_3354 += display_name;
                    _msg_3354 += " auto&&') -- an rvalue-reference parameter always takes ownership via ";
                    _msg_3354 += "move (ch03), which needs mutable access to the value being moved from";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_3354));
                }
            }
            auto pointee = std::make_shared<Type>(std::move(type));
            Type reference_type{};
            reference_type.kind = TypeKind::Reference;
            reference_type.pointee = std::move(pointee);
            reference_type.is_mutable_ref = true;
            reference_type.is_rvalue_ref = true;
            return reference_type;
        }
        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(type));
            pointee->is_const_qualified = has_const;
            Type reference_type{};
            reference_type.kind = TypeKind::Reference;
            reference_type.pointee = std::move(pointee);
            reference_type.is_mutable_ref = !has_const;
            return reference_type;
        }
        // `const ConceptName auto v` / `const auto v` -- by value, and
        // `const` at the top level of a parameter, which [dcl.fct]/5
        // deletes from the function type. set_param_declared_type moves
        // it onto Param::is_const; leaving it here is what makes that
        // happen, and what makes an assignment to `v` in the body an
        // error rather than a silent write.
        type.is_const_qualified = has_const;
        return type; // "ConceptName auto"/"auto" -- by value
    }

    // Wraps `base` in Array types for each trailing `[N]` found after a
    // declared name (e.g. the `[8]` in `int values[8];`). Arrays of
    // references aren't valid C++ (there's no storage layout for a raw
    // reference), so reject up front rather than let it silently codegen
    // as an array of addresses.
    //
    // ch05 §9.4: the bracketed size is an arbitrary constant-expression
    // (not just an integer-literal token), parsed here exactly like any
    // other expression -- via the same `parse_expr()` entry point
    // `alignas(...)`'s own operand uses (parse_alignment_specifier_seq).
    // It is stored, unevaluated, in the new Array Type's
    // `array_size_expr`; the constexpr engine's array-bound resolution
    // pass (constexpression.cppm) evaluates and validates it later (or, for a
    // template-parameter-dependent bound such as `sizeof(T)`, at each
    // point of instantiation -- see monomorphize.cppm), then fills in
    // `array_size` and clears `array_size_expr`.
    //
    // Multi-dimensional declarators bind left-to-right ([dcl.array], adopted
    // unchanged by ch05 §9.4(1)): `int a[2][3]` is "array of 2 arrays of 3
    // int", so the FIRST bracket names the outermost bound. The brackets are
    // therefore collected in source order and the Type built up from the last
    // one inward -- wrapping `base` as each bracket is read would make the
    // last bracket outermost and silently transpose every bound.
    [[nodiscard]] std::expected<Type, ParseError> parse_array_suffix(Type base) {
        // ch00 §2/ch01 §1.3: `[[` (a doubled bracket) starts an
        // attribute-specifier-seq, never an array declarator -- stop
        // here rather than misparsing e.g. `T&& f [[scpp::thread_movable]]`
        // as if `[[scpp::thread_movable]]` were an (invalid, since `f`'s
        // own type is a Reference) array-of-references suffix. A real
        // array declarator's own size is always a single `[` (never
        // doubled), so this check never rejects a legitimate one.
        std::vector<std::shared_ptr<Expr>> bounds{};
        while (check(TokenKind::LBracket) && peek_at(1).kind != TokenKind::LBracket) {
            const Token& bracket_tok = peek();
            if (base.kind == TypeKind::Reference) {
                return std::unexpected(ParseError(bracket_tok.line, bracket_tok.column, "arrays of references are not supported"));
            }
            advance();
            auto size_expr_result = parse_expr();
            if (!size_expr_result.has_value()) return std::unexpected(std::move(size_expr_result).error());
            ExprPtr size_expr = std::move(size_expr_result).value();
            if (auto _r = expect(TokenKind::RBracket, "']'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            bounds.push_back(std::shared_ptr<Expr>(std::move(size_expr)));
        }
        for (std::size_t i = bounds.size(); i > 0; --i) {
            Type array_type{};
            array_type.kind = TypeKind::Array;
            array_type.element = std::make_shared<Type>(std::move(base));
            array_type.array_size_expr = bounds[i - 1];
            base = std::move(array_type);
        }
        return base;
    }

    [[nodiscard]] bool starts_function_pointer_declarator() const {
        return check(TokenKind::LParen) && peek_at(1).kind == TokenKind::Star;
    }

    [[nodiscard]] std::expected<std::vector<Type>, ParseError> parse_function_pointer_param_types() {
        std::vector<Type> params{};

        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!check(TokenKind::RParen)) {
            while (true) {
                auto param_type_result = parse_type(/*allow_rvalue_ref=*/true);
                if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                Type param_type = std::move(param_type_result).value();
                if (match(TokenKind::Ellipsis)) {
                    if (!referenced_pack_type_param_name(param_type).has_value()) {
                        const Token& tok = peek();
                        {
                            std::string _msg_3445{"a function-pointer parameter pack must name an enclosing type or "};
                            _msg_3445 += "function template parameter pack";
                            return std::unexpected(ParseError(tok.line, tok.column,
                                          _msg_3445));
                        }
                    }
                    param_type.is_pack_expansion = true;
                    if (check(TokenKind::Identifier)) advance(); // optional parameter name, ignored in a function type
                    if (!check(TokenKind::RParen)) {
                        const Token& tok = peek();
                        return std::unexpected(ParseError(tok.line, tok.column,
                                          "a function-pointer parameter pack must be the last parameter in the list"));
                    }
                } else if (check(TokenKind::Identifier)) {
                    advance(); // optional parameter name, ignored in a function type
                }
                auto param_type_suffix_result = parse_array_suffix(param_type);
                if (!param_type_suffix_result.has_value()) return std::unexpected(std::move(param_type_suffix_result).error());
                param_type = std::move(param_type_suffix_result).value();
                if (param_type.kind == TypeKind::Array) {
                    Type decayed{};

                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                params.push_back(std::move(param_type));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return params;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_function_type_suffix(Type return_type) {
        Type type{};

        type.kind = TypeKind::Function;
        type.function_return = std::make_shared<Type>(std::move(return_type));
        auto _tmp_result = parse_function_pointer_param_types();
        if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
        type.function_params = std::move(_tmp_result).value();
        type.is_const_function = match(TokenKind::KwConst);
        if (match(TokenKind::AmpAmp)) {
            type.function_ref_qualifier = ReceiverRefQualifier::RValue;
        } else if (match(TokenKind::Amp)) {
            type.function_ref_qualifier = ReceiverRefQualifier::LValue;
        }
        return type;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_template_type_argument() {
        auto type_result = parse_type(/*allow_rvalue_ref=*/false, /*out_bare_const=*/nullptr);
        if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
        Type type = std::move(type_result).value();
        const Token& attr_start_tok = peek();
        auto attrs_result = parse_attribute_specifier_seq();
        if (!attrs_result.has_value()) return std::unexpected(std::move(attrs_result).error());
        ParsedAttributes attrs = std::move(attrs_result).value();
        if (attrs.lifetime.present() && !allow_type_lifetime_attributes_) {
            {
                std::string _msg_3504{"'[[scpp::lifetime(name)]]' cannot appertain to a type-id here -- only to an eligible "};
                _msg_3504 += "parameter declaration or function declarator";
                return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                             _msg_3504));
            }
        }
        if (auto _rv = merge_lifetime_attribute(type.lifetime, attrs.lifetime, attr_start_tok,
                                 "a type-id within a parameter or return type"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (check(TokenKind::LParen)) {
            auto suffix_result = parse_function_type_suffix(std::move(type));
            if (!suffix_result.has_value()) return std::unexpected(std::move(suffix_result).error());
            type = std::move(suffix_result).value();
        }
        return type;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_function_pointer_declarator(Type return_type, std::string& out_name) {
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Star, "'*'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        const Token& attr_start_tok = peek();
        auto ptr_attrs_result = parse_attribute_specifier_seq();
        if (!ptr_attrs_result.has_value()) return std::unexpected(std::move(ptr_attrs_result).error());
        ParsedAttributes ptr_attrs = std::move(ptr_attrs_result).value();
        if (auto _rv = reject_packed_attribute(ptr_attrs, attr_start_tok, "a function-pointer declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        auto fn_ptr_name_result = expect(TokenKind::Identifier, "function pointer name");
        if (!fn_ptr_name_result.has_value()) return std::unexpected(std::move(fn_ptr_name_result).error());
        out_name = std::string(fn_ptr_name_result.value().text.data(), fn_ptr_name_result.value().text.size());
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        Type type{};

        type.kind = TypeKind::FunctionPointer;
        type.function_return = std::make_shared<Type>(std::move(return_type));
        auto _tmp_result = parse_function_pointer_param_types();
        if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
        type.function_params = std::move(_tmp_result).value();
        type.is_unsafe_function_pointer = ptr_attrs.has("unsafe");
        return type;
    }

    [[nodiscard]] std::expected<Type, ParseError> parse_named_declarator(Type base_type, std::string& out_name, const char* name_what) {
        if (starts_function_pointer_declarator()) {
            return parse_function_pointer_declarator(std::move(base_type), out_name);
        }
        auto name_result = expect(TokenKind::Identifier, name_what);
        if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
        out_name = std::string(name_result.value().text.data(), name_result.value().text.size());
        auto declared_type_result = parse_array_suffix(base_type);
        if (!declared_type_result.has_value()) return std::unexpected(std::move(declared_type_result).error());
        Type declared_type = std::move(declared_type_result).value();
        if (declared_type.kind == TypeKind::Array) {
            Type decayed{};

            decayed.kind = TypeKind::Pointer;
            decayed.pointee = declared_type.element;
            return decayed;
        }
        return declared_type;
    }

    // ch11 §11.3/§11.4: parses an optional module declaration at the very
    // start of the file -- `export module <dotted.name>[:<partition>];`
    // (a primary interface unit, or -- with the `:partition` suffix -- an
    // interface partition, either way may contain `export`-marked
    // declarations) or `module <dotted.name>[:<partition>];` (an
    // implementation unit/partition, contributes more code to the same
    // module but exports nothing of its own). Entirely absent for an
    // ordinary, non-module file (today's default, still the
    // overwhelmingly common case): nothing is consumed,
    // `program.module_name` stays empty and every existing behavior is
    // unaffected.
    [[nodiscard]] std::expected<void, ParseError> parse_module_declaration(Program& program) {
        bool saw_global_module_fragment = false;
        if (check(TokenKind::KwModule) && peek_at(1).kind == TokenKind::Semicolon) {
            advance(); // 'module'
            advance(); // ';'
            saw_global_module_fragment = true;
        }
        bool leading_export = check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::KwModule;
        if (!leading_export && !check(TokenKind::KwModule)) {
            if (saw_global_module_fragment) {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column,
                                 "a global module fragment ('module;') must be followed by a module declaration"));
            }
            return {};
        }
        if (leading_export) advance(); // 'export'
        advance(); // 'module'
        auto module_name_tok_result = expect(TokenKind::Identifier, "module name");
        if (!module_name_tok_result.has_value()) return std::unexpected(std::move(module_name_tok_result).error());
        std::string dotted{module_name_tok_result.value().text.data(), module_name_tok_result.value().text.size()};
        while (match(TokenKind::Dot)) {
            dotted.push_back('.');
            auto segment_result = expect(TokenKind::Identifier, "module name segment");
            if (!segment_result.has_value()) return std::unexpected(std::move(segment_result).error());
            dotted += std::string(segment_result.value().text.data(), segment_result.value().text.size());
        }
        // ch11 §11.4: an optional `:partition` suffix -- designates this
        // file as one specific partition of `dotted`, rather than its
        // primary interface/implementation unit. partition_name stays
        // separate from module_name (which always holds just the base
        // dotted name) so the export/namespace validation pass (§11.6)
        // keeps comparing against the *module's* own name, unaffected by
        // which partition happens to be declaring something.
        if (match(TokenKind::Colon)) {
            auto partition_name_result = expect(TokenKind::Identifier, "partition name");
            if (!partition_name_result.has_value()) return std::unexpected(std::move(partition_name_result).error());
            program.partition_name = std::string(partition_name_result.value().text.data(), partition_name_result.value().text.size());
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        program.module_name = dotted;
        program.is_module_interface = leading_export;
        program.is_module_impl = !leading_export;
        return {};
    }

    // ch11 §11.4/§11.8: parses zero or more `import name;` / `export
    // import name;` (cross-module, a dotted name) or `import :part;` /
    // `export import :part;` (ch11 §11.4, a same-module partition --
    // just a bare identifier, never dotted) declarations, immediately
    // resolving each via resolver_/partition_resolver_ (given by the
    // driver, which knows about `--import name=path` mappings and file
    // I/O -- the parser itself never touches the filesystem) so the
    // imported names are visible (struct_names_/class_names_) to the
    // rest of this file, which is parsed next -- mirrors real C++20's
    // own requirement that imports precede every other declaration.
    [[nodiscard]] std::expected<void, ParseError> parse_import_declarations(Program& program) {
        for (;;) {
            bool is_reexport = check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::KwImport;
            bool is_plain_import = check(TokenKind::KwImport);
            if (!is_reexport && !is_plain_import) return {};
            if (is_reexport) advance(); // 'export'
            const Token& import_tok = peek();
            advance(); // 'import'

            if (match(TokenKind::Colon)) {
                // ch11 §11.4: a same-module partition import -- only
                // meaningful inside a file that is itself part of some
                // module (primary unit or another partition).
                auto partition_name_tok_result = expect(TokenKind::Identifier, "partition name");
                if (!partition_name_tok_result.has_value()) return std::unexpected(std::move(partition_name_tok_result).error());
                std::string partition_name{partition_name_tok_result.value().text.data(), partition_name_tok_result.value().text.size()};
                if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

                if (program.module_name.empty()) {
                    {
                        std::string _msg_3647{"cannot import partition ':"};
                        _msg_3647 += partition_name;
                        _msg_3647 += "' -- this file has no 'module'/'export module' declaration of ";
                        _msg_3647 += "its own (ch11 §11.4: partitions only exist within a module)";
                        return std::unexpected(ParseError(import_tok.line, import_tok.column,
                                      _msg_3647));
                    }
                }
                ImportDecl import_decl{};

                import_decl.module_name = partition_name;
                import_decl.is_reexport = is_reexport;
                import_decl.is_partition = true;
                program.imports.push_back(import_decl);

                std::string key{program.module_name};
                key += ":";
                key += partition_name;
                auto resolved_partition_r = partition_resolver_(key);
                if (!resolved_partition_r.has_value()) {
                    ParseError resolver_error = std::move(resolved_partition_r).error();
                    // A known loc means partition_resolver_ is forwarding
                    // a real, already-positioned ParseError from a
                    // *nested* parse (e.g. a genuine syntax error inside
                    // the partition file itself) -- that position is
                    // strictly more useful than this import statement's
                    // own, so it is forwarded verbatim rather than
                    // rewritten. An unknown loc means the failure is
                    // resolver-native (circular import, cannot find
                    // partition, etc., or no_partition_resolver's own
                    // fallback above) with no position of its own yet --
                    // this import statement's own position is the best
                    // one available, so it is stamped on here.
                    if (resolver_error.loc.is_known()) return std::unexpected(std::move(resolver_error));
                    std::string _msg_3661{"cannot resolve partition '"};
                    _msg_3661 += key;
                    _msg_3661 += "': ";
                    _msg_3661 += resolver_error.what();
                    return std::unexpected(ParseError(import_tok.line, import_tok.column,
                                  _msg_3661));
                }
                Program resolved_partition = std::move(resolved_partition_r).value();
                if (resolved_partition.module_name.empty()) {
                    // Defensive: a resolver reporting success (an
                    // engaged std::expected) must still hand back a
                    // real, non-empty partition -- see PartitionResolver's
                    // own comment for why this invariant is checked
                    // rather than merely trusted.
                    {
                        std::string _msg_3661b{"partition resolver returned no result for '"};
                        _msg_3661b += key;
                        _msg_3661b += "' (see the driver's --import ";
                        _msg_3661b += key;
                        _msg_3661b += "=path flag)";
                        return std::unexpected(ParseError(import_tok.line, import_tok.column,
                                      _msg_3661b));
                    }
                }
                if (auto _rv = merge_partition(program, std::move(resolved_partition), import_decl.is_reexport, key, import_tok); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                continue;
            }

            std::string dotted{};

            {
                auto module_name_tok_result = expect(TokenKind::Identifier, "imported module name");
                if (!module_name_tok_result.has_value()) return std::unexpected(std::move(module_name_tok_result).error());
                dotted = std::string(module_name_tok_result.value().text.data(), module_name_tok_result.value().text.size());
            }
            while (match(TokenKind::Dot)) {
                dotted.push_back('.');
                auto segment_result = expect(TokenKind::Identifier, "module name segment");
                if (!segment_result.has_value()) return std::unexpected(std::move(segment_result).error());
                dotted += std::string(segment_result.value().text.data(), segment_result.value().text.size());
            }
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

            ImportDecl import_decl{};

            import_decl.module_name = dotted;
            import_decl.is_reexport = is_reexport;
            program.imports.push_back(std::move(import_decl));

            auto imported_r = resolver_(dotted);
            if (!imported_r.has_value()) {
                ParseError resolver_error = std::move(imported_r).error();
                // See the matching partition-import branch above for
                // why loc.is_known() is the signal distinguishing a
                // forwarded, already-positioned nested ParseError
                // from a resolver-native failure needing this import
                // statement's own position stamped on.
                if (resolver_error.loc.is_known()) return std::unexpected(std::move(resolver_error));
                std::string _msg_3693{"cannot resolve imported module '"};
                _msg_3693 += dotted;
                _msg_3693 += "': ";
                _msg_3693 += resolver_error.what();
                return std::unexpected(ParseError(import_tok.line, import_tok.column, _msg_3693));
            }
            const Program* imported_ptr = imported_r.value();
            if (imported_ptr == nullptr) {
                // Defensive: a resolver reporting success (an engaged
                // std::expected) must still hand back a real, non-null
                // Program -- see ModuleResolver's own comment for why
                // this invariant is checked rather than merely trusted.
                std::string _msg_3693b{"module resolver returned no result for '"};
                _msg_3693b += dotted;
                _msg_3693b += "' (see the driver's --import name=path flag)";
                return std::unexpected(ParseError(import_tok.line, import_tok.column, _msg_3693b));
            }
            // imported_ptr is known non-null here (checked just above);
            // self-hosting still requires an explicit `[[scpp::unsafe]] { }`
            // to dereference any raw pointer (ch01 §1.3/ch02).
            [[scpp::unsafe]] {
                if (auto _rv = merge_imported_module(program, *imported_ptr, dotted, is_reexport); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
        }
    }

    // Manually clones a Function (including every semantic flag the
    // movechecker/parser rely on) since Function::body is a unique_ptr and
    // so Function itself has no implicit copy constructor.
    //
    // Imported *ordinary* functions keep only their declaration surface:
    // the defining module's own separately-compiled object file provides
    // the body. Imported *templates*, though, need their body cloned into
    // the importer so the importer's own later monomorphization can
    // instantiate concrete copies locally (exactly like a C++ template
    // definition being reachable from an import). `keep_body` selects
    // between those two cases.
    //
    // `fallback_owning_module` is only used when `fn` doesn't already have
    // an owning_module of its own (i.e. `fn` is `imported`'s own local
    // declaration, not itself a pass-through re-export -- see
    // merge_imported_module's own comment for why preserving an already-set
    // owning_module matters).
    // `is_reexport` gates whether the clone stays exported at all (ch11
    // §11.8: a private, non-reexporting import must not forward what it
    // sees to whoever imports the *current* file in turn).
    Function clone_function_declaration(const Function& fn, const std::string& fallback_owning_module,
                                        bool is_reexport, bool keep_body) {
        Function clone{};

        clone.return_type = fn.return_type;
        clone.name = fn.name;
        clone.loc = fn.loc;
        for (const Param& param : fn.params) clone.params.push_back(deep_clone_param(param));
        clone.return_lifetime = fn.return_lifetime;
        if (keep_body && fn.body != nullptr) clone.body = deep_clone_stmt(*fn.body);
        clone.is_extern_c = fn.is_extern_c;
        clone.is_module_extern = fn.is_module_extern;
        clone.is_unsafe = fn.is_unsafe;
        clone.is_nodiscard = fn.is_nodiscard;
        clone.nodiscard_reason = fn.nodiscard_reason;
        clone.is_compile_time_dependency = fn.is_compile_time_dependency;
        clone.skip_imported_body_verification = fn.skip_imported_body_verification;
        clone.eval_mode = fn.eval_mode;
        clone.has_varargs = fn.has_varargs;
        clone.method_requires_concept = fn.method_requires_concept;
        clone.is_generic_template = fn.is_generic_template;
        clone.template_params = fn.template_params;
        clone.generic_method_owner_id = fn.generic_method_owner_id;
        clone.member_owner_class = fn.member_owner_class;
        clone.member_initializers = fn.member_initializers;
        clone.receiver_ref_qualifier = fn.receiver_ref_qualifier;
        clone.is_static = fn.is_static;
        clone.access = fn.access;
        clone.is_virtual = fn.is_virtual;
        clone.is_override = fn.is_override;
        clone.is_pure = fn.is_pure;
        clone.is_defaulted = fn.is_defaulted;
        clone.is_deleted = fn.is_deleted;
        clone.expects_out_of_line_definition = false;
        clone.forwards_to = fn.forwards_to;
        clone.namespace_path = fn.namespace_path;
        clone.is_exported = is_reexport && fn.is_exported;
        clone.owning_module = fn.owning_module.empty() ? std::string(fallback_owning_module) : fn.owning_module;
        clone.visibility_module = fn.visibility_module.empty() ? clone.owning_module : fn.visibility_module;
        return clone;
    }

    [[nodiscard]] std::string next_generic_template_owner_id() {
        {
            std::string _msg_3762{"__gtpl"};
            _msg_3762 += std::to_string(parser_instance_id_);
            _msg_3762 += "_";
            _msg_3762 += std::to_string(++generic_template_owner_counter_);
            return _msg_3762;
        }
    }

    // Merges `imported`'s exported surface into the Program currently
    // being parsed (ch11 §11.8): every exported StructDef/ClassDef/
    // Function is cloned in. Each clone's `owning_module` is set to
    // `imported_name` *only if the original declaration didn't already
    // have one* -- a declaration that reached `imported` itself via a
    // transitive `export import` (e.g. `imported` is "b", which did
    // `export import a;`, so this Function's owning_module is already
    // "a") must keep pointing at its *original* defining module, not get
    // overwritten with "b": codegen's mangling scheme (keyed off
    // owning_module) has to match whatever "a"'s own separate
    // compilation actually defines, regardless of how many modules
    // re-exported it along the way. `is_reexport` (true for `export
    // import name;`, false for a plain `import name;`) gates whether the
    // clone stays exported at all: a private import must not forward
    // what it sees to whoever imports the *current* file in turn (ch11
    // §11.8's own "private, non-transitive" rule) -- `struct_names_`/
    // `class_names_` registration is unaffected either way, since that's
    // about the type being usable in *this* file's own subsequent
    // parsing, not about further forwarding. Imported template
    // definitions do keep their body (see clone_function_declaration
    // above) so the importer can monomorphize them locally; ordinary,
    // non-template functions stay declaration-only and are defined by the
    // imported module's own object file. Only `is_exported`
    // declarations are visible to an importer at all -- a module-private
    // helper is invisible outside its own file, matching real C++20
    // modules.
    [[nodiscard]] std::expected<void, ParseError> merge_imported_module(Program& program, const Program& imported, const std::string& imported_name,
                                bool is_reexport) {
        std::unordered_set<std::string> hidden_function_designators = imported_hidden_function_designators(imported);
        for (const EnumDef& enum_def : imported.enums) {
            std::string effective_owner{};
            if (enum_def.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = enum_def.owning_module;
            }
            if (!enum_def.is_exported && effective_owner != imported_name) continue;
            if (!enum_def.is_exported && !enum_def.is_compile_time_dependency) continue;
            if (enum_def.is_exported) struct_names_.insert(enum_def.name);
            EnumDef* existing_enum = nullptr;
            for (std::size_t i = 0; i < program.enums.size(); i++) {
                if (program.enums[i].owning_module == effective_owner && same_enum_identity(program.enums[i], enum_def)) {
                    existing_enum = &program.enums[i];
                    break;
                }
            }
            if (existing_enum != nullptr) {
                // existing_enum is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_enum->is_exported = existing_enum->is_exported || (is_reexport && enum_def.is_exported);
                    existing_enum->is_compile_time_dependency = existing_enum->is_compile_time_dependency || enum_def.is_compile_time_dependency;
                    continue;
                }
            }
            EnumDef clone_enum = enum_def;
            if (clone_enum.owning_module.empty()) clone_enum.owning_module = imported_name;
            clone_enum.is_exported = is_reexport && clone_enum.is_exported;
            program.enums.push_back(std::move(clone_enum));
        }
        for (const StructDef& struct_def : imported.structs) {
            std::string effective_owner{};
            if (struct_def.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = struct_def.owning_module;
            }
            if (!struct_def.is_exported && effective_owner != imported_name) continue;
            if (!struct_def.is_exported && !struct_def.is_compile_time_dependency) continue;
            if (struct_def.is_exported) {
                struct_names_.insert(struct_def.name);
                if (auto _rv = register_record_tag_kind(struct_def.name, struct_def.is_union ? RecordTagKind::Union : RecordTagKind::Struct, struct_def.loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            if (struct_def.is_exported && !struct_def.template_params.empty()) {
                generic_type_names_.insert(struct_def.name);
                ordinary_generic_type_template_params_.insert_or_assign(struct_def.name, struct_def.template_params);
            }
            StructDef* existing_struct = nullptr;
            for (std::size_t i = 0; i < program.structs.size(); i++) {
                if (program.structs[i].owning_module == effective_owner && same_struct_identity(program.structs[i], struct_def)) {
                    existing_struct = &program.structs[i];
                    break;
                }
            }
            if (existing_struct != nullptr) {
                // existing_struct is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_struct->is_exported = existing_struct->is_exported || (is_reexport && struct_def.is_exported);
                    existing_struct->is_compile_time_dependency = existing_struct->is_compile_time_dependency || struct_def.is_compile_time_dependency;
                    continue;
                }
            }
            StructDef clone_struct = struct_def;
            if (clone_struct.owning_module.empty()) clone_struct.owning_module = imported_name;
            clone_struct.is_exported = is_reexport && clone_struct.is_exported;
            program.structs.push_back(std::move(clone_struct));
        }
        for (const ClassDef& class_def : imported.classes) {
            std::string effective_owner{};
            if (class_def.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = class_def.owning_module;
            }
            if (!class_def.is_exported && effective_owner != imported_name) continue;
            if (!class_def.is_exported && !class_def.is_compile_time_dependency) continue;
            if (class_def.is_exported) {
                struct_names_.insert(class_def.name);
                class_names_.insert(class_def.name);
                if (auto _rv = register_record_tag_kind(class_def.name, RecordTagKind::Class, class_def.loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            if (class_def.is_exported && (!class_def.template_params.empty() || class_def.is_variadic_primary_template)) {
                generic_type_names_.insert(class_def.name);
                if (class_def.is_variadic_primary_template) {
                    variadic_primary_template_params_.insert_or_assign(class_def.name, class_def.template_params);
                } else if (!class_def.is_partial_specialization) {
                    ordinary_generic_type_template_params_.insert_or_assign(class_def.name, class_def.template_params);
                }
            }
            ClassDef* existing_class = nullptr;
            for (std::size_t i = 0; i < program.classes.size(); i++) {
                if (program.classes[i].owning_module == effective_owner && same_class_identity(program.classes[i], class_def)) {
                    existing_class = &program.classes[i];
                    break;
                }
            }
            if (existing_class != nullptr) {
                // existing_class is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_class->is_exported = existing_class->is_exported || (is_reexport && class_def.is_exported);
                    existing_class->is_compile_time_dependency = existing_class->is_compile_time_dependency || class_def.is_compile_time_dependency;
                    continue;
                }
            }
            ClassDef clone_class = clone_class_def(class_def);
            if (clone_class.owning_module.empty()) clone_class.owning_module = imported_name;
            clone_class.is_exported = is_reexport && clone_class.is_exported;
            program.classes.push_back(std::move(clone_class));
        }
        for (const TypeAliasDecl& alias : imported.type_aliases) {
            if (!alias.is_exported) continue;
            type_aliases_.insert_or_assign(alias.name, alias.underlying_type);
            std::string effective_owner{};
            if (alias.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = alias.owning_module;
            }
            TypeAliasDecl* existing_alias = nullptr;
            for (std::size_t i = 0; i < program.type_aliases.size(); i++) {
                if (program.type_aliases[i].owning_module == effective_owner && program.type_aliases[i].name == alias.name &&
                    types_equal(program.type_aliases[i].underlying_type, alias.underlying_type)) {
                    existing_alias = &program.type_aliases[i];
                    break;
                }
            }
            if (existing_alias != nullptr) {
                // existing_alias is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_alias->is_exported = existing_alias->is_exported || (is_reexport && alias.is_exported);
                    continue;
                }
            }
            TypeAliasDecl clone_alias = alias;
            if (clone_alias.owning_module.empty()) clone_alias.owning_module = imported_name;
            clone_alias.is_exported = is_reexport && clone_alias.is_exported;
            program.type_aliases.push_back(std::move(clone_alias));
        }
        // See merge_partition's identical comment -- a concept exported
        // from a genuinely separate module (e.g. `import std;` picking
        // up std::copy_constructible) needs the same treatment an
        // ordinary exported struct/class/type-alias already gets here:
        // registered into concept_names_ and program.concepts so both
        // name resolution and later concept-satisfaction checking can
        // see it, gated by is_exported exactly like every other cross-
        // module symbol above.
        for (const ConceptDef& concept_def : imported.concepts) {
            std::string effective_owner{};
            if (concept_def.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = concept_def.owning_module;
            }
            if (!concept_def.is_exported) continue;
            concept_names_.insert(concept_def.name);
            ConceptDef* existing_concept = nullptr;
            for (std::size_t i = 0; i < program.concepts.size(); i++) {
                if (program.concepts[i].owning_module == effective_owner && program.concepts[i].name == concept_def.name) {
                    existing_concept = &program.concepts[i];
                    break;
                }
            }
            if (existing_concept != nullptr) {
                // existing_concept is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_concept->is_exported = existing_concept->is_exported || (is_reexport && concept_def.is_exported);
                    continue;
                }
            }
            ConceptDef clone_concept = concept_def;
            if (clone_concept.owning_module.empty()) clone_concept.owning_module = imported_name;
            clone_concept.is_exported = is_reexport && clone_concept.is_exported;
            program.concepts.push_back(std::move(clone_concept));
        }
        for (const Function& fn : imported.functions) {
            std::string effective_owner{};
            if (fn.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = fn.owning_module;
            }
            if (!fn.is_exported && effective_owner != imported_name) continue;
            bool keep_body = imported_function_body_must_stay_available(imported, fn);
            bool needs_hidden_compile_time_visibility =
                !fn.is_exported && hidden_function_designators.contains(fn.name);
            if (!fn.is_exported && !fn.is_compile_time_dependency && !needs_hidden_compile_time_visibility) continue;
            if (fn.is_exported && !fn.template_params.empty()) generic_function_template_params_.insert_or_assign(fn.name, fn.template_params);
            Function* existing_fn = nullptr;
            for (std::size_t i = 0; i < program.functions.size(); i++) {
                if (program.functions[i].owning_module == effective_owner &&
                    program.functions[i].loc.source_path_text() == fn.loc.source_path_text() &&
                    program.functions[i].loc.line == fn.loc.line && program.functions[i].loc.column == fn.loc.column &&
                    same_function_signature(program.functions[i], fn)) {
                    existing_fn = &program.functions[i];
                    break;
                }
            }
            if (existing_fn != nullptr) {
                // existing_fn is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_fn->is_exported = existing_fn->is_exported || (is_reexport && fn.is_exported);
                    existing_fn->is_compile_time_dependency =
                        existing_fn->is_compile_time_dependency || fn.is_compile_time_dependency || needs_hidden_compile_time_visibility;
                    if (existing_fn->body == nullptr && keep_body && fn.body != nullptr) existing_fn->body = deep_clone_stmt(*fn.body);
                    if (existing_fn->body != nullptr && existing_fn->is_compile_time_dependency &&
                        existing_fn->eval_mode == FunctionEvalMode::RuntimeOnly) {
                        existing_fn->skip_imported_body_verification = true;
                }
                continue;
                }
            }
            Function clone_fn = clone_function_declaration(fn, imported_name, is_reexport, keep_body);
            clone_fn.is_compile_time_dependency = clone_fn.is_compile_time_dependency || needs_hidden_compile_time_visibility;
            if (clone_fn.body != nullptr && clone_fn.is_compile_time_dependency && clone_fn.eval_mode == FunctionEvalMode::RuntimeOnly) {
                clone_fn.skip_imported_body_verification = true;
            }
            program.functions.push_back(std::move(clone_fn));
        }
        for (const GlobalVar& global : imported.globals) {
            if (!global.is_exported) continue;
            std::string effective_owner{};
            if (global.owning_module.empty()) {
                effective_owner = imported_name;
            } else {
                effective_owner = global.owning_module;
            }
            GlobalVar* existing_global = nullptr;
            for (std::size_t i = 0; i < program.globals.size(); i++) {
                if (program.globals[i].owning_module == effective_owner && program.globals[i].decl != nullptr &&
                    global.decl != nullptr && program.globals[i].decl->var_name == global.decl->var_name) {
                    existing_global = &program.globals[i];
                    break;
                }
            }
            if (existing_global != nullptr) {
                // existing_global is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_global->is_exported = existing_global->is_exported || (is_reexport && global.is_exported);
                    continue;
                }
            }
            GlobalVar clone_global = clone_global_var(global);
            if (clone_global.owning_module.empty()) clone_global.owning_module = imported_name;
            clone_global.is_exported = is_reexport && clone_global.is_exported;
            program.globals.push_back(std::move(clone_global));
        }
        return {};
    }

    // Merges *every* declaration (exported or not -- ch11 §11.4: within a
    // module, any unit that imports a partition sees everything in it) of
    // `partition` into the Program currently being parsed. Unlike
    // merge_imported_module (which clones a cross-module import's
    // exported-only surface, always clearing Function bodies since that
    // module compiles *separately*), this genuinely moves each
    // StructDef/ClassDef/Function -- bodies included -- out of
    // `partition`, since a partition compiles *together* with whatever
    // imports it, as one combined unit (ch11 §11.4's own framing).
    // A partition's own local declarations keep their default-empty
    // `owning_module` (so once merged they become this Program's own
    // local declarations), but anything the partition itself imported
    // from some *other* module must keep that pre-existing
    // `owning_module` exactly as-is -- otherwise codegen would later
    // "forget" those declarations are foreign and emit plain local symbol
    // references instead of the imported module's real mangled names.
    //
    // `is_reexport` (true for `export import :part;`, false for a plain
    // `import :part;`) controls whether the partition's own individual
    // `export` markings survive into the merged copy (so they become
    // part of the *whole module's* external export surface) or are
    // forced false (so the partition's content stays usable inside the
    // module -- this file and its sibling partitions -- but invisible to
    // anyone importing the module from outside). Attempting `export
    // import` on an implementation partition (`module name:part;`, no
    // `export` on its own module declaration) is rejected: such a
    // partition can never export anything to the outside, by
    // construction, matching real C++20.
    [[nodiscard]] std::expected<void, ParseError> merge_partition(Program& program, Program&& partition, bool is_reexport, const std::string& key,
                          const Token& import_tok) {
        if (is_reexport && partition.is_module_impl) {
            {
                std::string _msg_4019{"cannot 'export import' partition '"};
                _msg_4019 += key;
                _msg_4019 += "': it is an implementation partition ('module ...;' with no 'export' on ";
                _msg_4019 += "its own module declaration), so it can never export anything to the ";
                _msg_4019 += "outside (ch11 §11.4)";
                return std::unexpected(ParseError(import_tok.line, import_tok.column,
                              _msg_4019));
            }
        }
        for (EnumDef& enum_def : partition.enums) {
            struct_names_.insert(enum_def.name);
            EnumDef* existing_enum = nullptr;
            for (std::size_t i = 0; i < program.enums.size(); i++) {
                if (program.enums[i].owning_module == enum_def.owning_module && same_enum_identity(program.enums[i], enum_def)) {
                    existing_enum = &program.enums[i];
                    break;
                }
            }
            if (existing_enum != nullptr) {
                // existing_enum is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_enum->is_exported = existing_enum->is_exported || (is_reexport && enum_def.is_exported);
                    existing_enum->is_compile_time_dependency = existing_enum->is_compile_time_dependency || enum_def.is_compile_time_dependency;
                    continue;
                }
            }
            enum_def.is_exported = is_reexport && enum_def.is_exported;
            program.enums.push_back(std::move(enum_def));
        }
        for (StructDef& struct_def : partition.structs) {
            struct_names_.insert(struct_def.name);
            if (auto _rv = register_record_tag_kind(struct_def.name, struct_def.is_union ? RecordTagKind::Union : RecordTagKind::Struct, struct_def.loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (!struct_def.template_params.empty()) {
                generic_type_names_.insert(struct_def.name);
                ordinary_generic_type_template_params_.insert_or_assign(struct_def.name, struct_def.template_params);
            }
            StructDef* existing_struct = nullptr;
            for (std::size_t i = 0; i < program.structs.size(); i++) {
                if (program.structs[i].owning_module == struct_def.owning_module && same_struct_identity(program.structs[i], struct_def)) {
                    existing_struct = &program.structs[i];
                    break;
                }
            }
            if (existing_struct != nullptr) {
                // existing_struct is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_struct->is_exported = existing_struct->is_exported || (is_reexport && struct_def.is_exported);
                    existing_struct->is_compile_time_dependency = existing_struct->is_compile_time_dependency || struct_def.is_compile_time_dependency;
                    continue;
                }
            }
            struct_def.is_exported = is_reexport && struct_def.is_exported;
            program.structs.push_back(std::move(struct_def));
        }
        for (ClassDef& class_def : partition.classes) {
            struct_names_.insert(class_def.name);
            class_names_.insert(class_def.name);
            if (auto _rv = register_record_tag_kind(class_def.name, RecordTagKind::Class, class_def.loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (!class_def.template_params.empty() || class_def.is_variadic_primary_template) {
                generic_type_names_.insert(class_def.name);
                if (class_def.is_variadic_primary_template) {
                    variadic_primary_template_params_.insert_or_assign(class_def.name, class_def.template_params);
                } else if (!class_def.is_partial_specialization) {
                    ordinary_generic_type_template_params_.insert_or_assign(class_def.name, class_def.template_params);
                }
            }
            ClassDef* existing_class = nullptr;
            for (std::size_t i = 0; i < program.classes.size(); i++) {
                if (program.classes[i].owning_module == class_def.owning_module && same_class_identity(program.classes[i], class_def)) {
                    existing_class = &program.classes[i];
                    break;
                }
            }
            if (existing_class != nullptr) {
                // existing_class is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_class->is_exported = existing_class->is_exported || (is_reexport && class_def.is_exported);
                    existing_class->is_compile_time_dependency = existing_class->is_compile_time_dependency || class_def.is_compile_time_dependency;
                    continue;
                }
            }
            class_def.is_exported = is_reexport && class_def.is_exported;
            program.classes.push_back(std::move(class_def));
        }
        for (TypeAliasDecl& alias : partition.type_aliases) {
            type_aliases_.insert_or_assign(alias.name, alias.underlying_type);
            TypeAliasDecl* existing_alias = nullptr;
            for (std::size_t i = 0; i < program.type_aliases.size(); i++) {
                if (program.type_aliases[i].owning_module == alias.owning_module && program.type_aliases[i].name == alias.name &&
                    types_equal(program.type_aliases[i].underlying_type, alias.underlying_type)) {
                    existing_alias = &program.type_aliases[i];
                    break;
                }
            }
            if (existing_alias != nullptr) {
                // existing_alias is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_alias->is_exported = existing_alias->is_exported || (is_reexport && alias.is_exported);
                    continue;
                }
            }
            alias.is_exported = is_reexport && alias.is_exported;
            program.type_aliases.push_back(std::move(alias));
        }
        // ch05 §5.11/ch11 §11.4: a concept declared in one partition of
        // this same module (e.g. std_concepts.scpp's own `:concepts`)
        // must be visible -- both for name resolution
        // (concept_names_, so resolve_visible_concept_name/parse_
        // optional_method_requires_clause and the abbreviated-form
        // lookups above can find it) and for concept-satisfaction
        // checking later (program.concepts, consulted by
        // monomorphize.cppm's own concepts_by_name_) -- to every *other*
        // partition of the same module (e.g. `:vector`), exactly like a
        // struct/class/type-alias declared in one partition already is.
        for (ConceptDef& concept_def : partition.concepts) {
            concept_names_.insert(concept_def.name);
            ConceptDef* existing_concept = nullptr;
            for (std::size_t i = 0; i < program.concepts.size(); i++) {
                if (program.concepts[i].owning_module == concept_def.owning_module && program.concepts[i].name == concept_def.name) {
                    existing_concept = &program.concepts[i];
                    break;
                }
            }
            if (existing_concept != nullptr) {
                // existing_concept is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_concept->is_exported = existing_concept->is_exported || (is_reexport && concept_def.is_exported);
                    continue;
                }
            }
            concept_def.is_exported = is_reexport && concept_def.is_exported;
            program.concepts.push_back(std::move(concept_def));
        }
        for (Function& fn : partition.functions) {
            Function* existing_fn = nullptr;
            for (std::size_t i = 0; i < program.functions.size(); i++) {
                if (program.functions[i].owning_module == fn.owning_module &&
                    program.functions[i].loc.source_path_text() == fn.loc.source_path_text() &&
                    program.functions[i].loc.line == fn.loc.line && program.functions[i].loc.column == fn.loc.column &&
                    same_function_signature(program.functions[i], fn)) {
                    existing_fn = &program.functions[i];
                    break;
                }
            }
            if (existing_fn != nullptr) {
                // existing_fn is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_fn->is_exported = existing_fn->is_exported || (is_reexport && fn.is_exported);
                    existing_fn->is_compile_time_dependency = existing_fn->is_compile_time_dependency || fn.is_compile_time_dependency;
                    continue;
                }
            }
            fn.is_exported = is_reexport && fn.is_exported;
            program.functions.push_back(std::move(fn));
        }
        for (GlobalVar& global : partition.globals) {
            GlobalVar* existing_global = nullptr;
            for (std::size_t i = 0; i < program.globals.size(); i++) {
                if (program.globals[i].owning_module == global.owning_module && program.globals[i].decl != nullptr &&
                    global.decl != nullptr && program.globals[i].decl->var_name == global.decl->var_name) {
                    existing_global = &program.globals[i];
                    break;
                }
            }
            if (existing_global != nullptr) {
                // existing_global is a raw pointer known non-null here (the
                // `!= nullptr` check just above), but self-hosting still
                // requires an explicit `[[scpp::unsafe]] { }` to dereference
                // any raw pointer (ch01 §1.3/ch02).
                [[scpp::unsafe]] {
                    existing_global->is_exported = existing_global->is_exported || (is_reexport && global.is_exported);
                    continue;
                }
            }
            global.is_exported = is_reexport && global.is_exported;
            program.globals.push_back(std::move(global));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_exported_top_level_entry(Program& program, bool inherited_export) {
        if (check(TokenKind::KwNamespace)) {
            if (auto _rv = parse_namespace_block(program, inherited_export); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            return {};
        }
        if (check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::LBrace) {
            // `export { <item> <item> ... }` -- groups several
            // declarations under one export marker (ch11 §11.3),
            // equivalent to writing `export` before each
            // individually.
            advance(); // 'export'
            advance(); // '{'
            while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                if (auto _rv = parse_exported_top_level_entry(program, /*inherited_export=*/true); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            return {};
        }
        bool is_exported = inherited_export || match(TokenKind::KwExport);
        if (check(TokenKind::KwNamespace)) {
            if (auto _rv = parse_namespace_block(program, is_exported); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            return {};
        }
        if (auto _rv = parse_top_level_item(program, is_exported); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        return {};
    }

    // The main "loop over top-level declarations" body, shared between
    // file scope (`inside_namespace=false`, terminated only by
    // EndOfFile -- a stray '}' here is left unconsumed, surfacing as an
    // ordinary parse error downstream exactly as it always has) and the
    // inside of a `namespace { ... }` block (`inside_namespace=true`,
    // also terminated by the block's own closing '}').
    [[nodiscard]] std::expected<void, ParseError> parse_top_level_items(Program& program, bool inside_namespace = false) {
        while (!check(TokenKind::EndOfFile) && !(inside_namespace && check(TokenKind::RBrace))) {
            if (auto _rv = parse_exported_top_level_entry(program, /*inherited_export=*/false); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        return {};
    }

    // Parses exactly one enum/struct/class/concept/function(-or-extern-group)
    // top-level item, given whether an `export` marker (individual, or
    // inherited from an enclosing `export { }` group) applies to it.
    [[nodiscard]] std::expected<void, ParseError> parse_top_level_item(Program& program, bool is_exported) {
        // ch01 §1.2/§1.3: a leading `[[scpp::unsafe]]` attribute-
        // specifier-seq, if any, applies to a function's own
        // declaration (the function-level unsafe marker) -- parsed once
        // here, before dispatching on what actually follows, since it's
        // only meaningful on a function (ordinary, `extern`, or a
        // full-header-form generic template), never on a struct/class/
        // concept declaration (ch01 §1.3 (1): "if an attribute-
        // specifier-seq containing the attribute-token unsafe
        // appertains to anything other than [a compound-statement or a
        // function], the program is ill-formed").
        auto leading_alignments_result = parse_alignment_specifier_seq();
        if (!leading_alignments_result.has_value()) return std::unexpected(std::move(leading_alignments_result).error());
        std::vector<AlignmentSpecifier> leading_alignments = std::move(leading_alignments_result).value();
        const Token& attr_start_tok = peek();
        auto leading_attrs_result = parse_attribute_specifier_seq();
        if (!leading_attrs_result.has_value()) return std::unexpected(std::move(leading_attrs_result).error());
        ParsedAttributes leading_attrs = std::move(leading_attrs_result).value();
        bool requested_unsafe = leading_attrs.has("unsafe");
        bool requested_packed = leading_attrs.has("packed");
        bool requested_nodiscard = leading_attrs.has_nodiscard;
        std::string requested_nodiscard_reason = leading_attrs.nodiscard_reason;
        auto reject_unsafe_if_requested = [&](const char* what) -> std::expected<void, ParseError> {
            if (requested_unsafe) {
                {
                    std::string _msg_4239{"\'[[scpp::unsafe]]\' cannot appertain to "};
                    _msg_4239 += std::string(what);
                    _msg_4239 += " -- only to a compound-statement or a function\'s own declaration ";
                    _msg_4239 += "(ch01 §1.3)";
                    return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                                  _msg_4239));
                }
            }
        return {};
        };
        if (check(TokenKind::KwUsing)) {
            if (auto _rv = reject_unsafe_if_requested("a type alias declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (auto _rv = reject_alignment_specifiers(leading_alignments, "a type alias declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "a type alias declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (requested_nodiscard) {
                return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                                 "'[[nodiscard]]' cannot appertain to a type alias declaration"));
            }
            if (auto _rv = parse_type_alias_decl(program, is_exported); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        } else if (check(TokenKind::KwStruct)) {
            if (auto _rv = reject_unsafe_if_requested("a 'struct' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            SourceLocation loc = current_loc();
            std::vector<GenericTypeParam> no_template_params{};
            auto struct_def_result = parse_struct_def(program, is_exported, std::move(no_template_params), std::move(leading_alignments));
            if (!struct_def_result.has_value()) return std::unexpected(std::move(struct_def_result).error());
            StructDef struct_def = std::move(struct_def_result).value();
            {
                std::string _msg_4261{"struct '"};
                _msg_4261 += struct_def.name;
                _msg_4261 += "'";
                if (auto _rv = check_export_context(program, is_exported, struct_def.namespace_path, loc, _msg_4261); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            program.structs.push_back(std::move(struct_def));
        } else if (check(TokenKind::KwEnum)) {
            if (auto _rv = reject_unsafe_if_requested("an 'enum class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (auto _rv = reject_alignment_specifiers(leading_alignments, "an 'enum class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "an 'enum class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            SourceLocation loc = current_loc();
            auto enum_def_result = parse_enum_def();
            if (!enum_def_result.has_value()) return std::unexpected(std::move(enum_def_result).error());
            EnumDef enum_def = std::move(enum_def_result).value();
            enum_def.is_exported = is_exported;
            {
                std::string _msg_4272{"enum class '"};
                _msg_4272 += enum_def.name;
                _msg_4272 += "'";
                if (auto _rv = check_export_context(program, is_exported, enum_def.namespace_path, loc, _msg_4272); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            program.enums.push_back(std::move(enum_def));
        } else if (check(TokenKind::KwUnion)) {
            if (auto _rv = reject_unsafe_if_requested("a 'union' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            SourceLocation loc = current_loc();
            auto union_def_result = parse_union_def(std::move(leading_alignments));
            if (!union_def_result.has_value()) return std::unexpected(std::move(union_def_result).error());
            StructDef union_def = std::move(union_def_result).value();
            union_def.is_exported = is_exported;
            {
                std::string _msg_4281{"union '"};
                _msg_4281 += union_def.name;
                _msg_4281 += "'";
                if (auto _rv = check_export_context(program, is_exported, union_def.namespace_path, loc, _msg_4281); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
            program.structs.push_back(std::move(union_def));
        } else if (check(TokenKind::KwClass)) {
            if (auto _rv = reject_unsafe_if_requested("a 'class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (requested_packed) {
                return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                                 "'[[scpp::packed]]' is only supported on struct/union declarations"));
            }
            std::vector<GenericTypeParam> no_template_params{};
            if (auto _rv = parse_class_def(program, is_exported, std::move(no_template_params), std::move(leading_alignments)); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        } else if (check(TokenKind::KwTemplate)) {
            // ch05 §5.11/§5.14: `template<...>` introduces either a
            // `concept` declaration or a generic `class`/`struct` type
            // -- peek past the whole header (of whatever length --
            // zero, one, or several parameters, possibly ending in a
            // pack) to see which of the three keywords follows, without
            // consuming anything yet.
            std::size_t after_header = offset_after_matching_angle(1); // peek_at(1) is the header's own '<'
            TokenKind after_header_kind = peek_at(after_header).kind;
            if (after_header_kind == TokenKind::KwClass) {
                if (auto _rv = reject_unsafe_if_requested("a 'class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "a 'class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                // A class name is immediately followed by `;` (a
                // variadic primary template's own bodyless forward
                // declaration, e.g. `template<typename... Ts> class
                // Tuple;`), `<` (one of the two fixed specializations of
                // an already-declared primary template, e.g.
                // `template<> class Tuple<> { ... };`), or `{`/`:` (an
                // ordinary, single-template-parameter generic class,
                // ch05 §5.14's phase-1 shape). A class-head attribute
                // (e.g. `[[nodiscard("...")]]`) may sit between `class`
                // and the name itself in any of these three cases, so
                // skip past it first (offset_after_attribute_specifier_seq
                // is a no-op when there is none) before locating the name
                // and the token right after it.
                std::size_t name_offset = offset_after_attribute_specifier_seq(after_header + 1);
                TokenKind after_name = peek_at(name_offset + 1).kind;
                if (after_name == TokenKind::Semicolon) {
                    auto template_params_result = parse_generic_type_header();
                    if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
                    std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
                    std::size_t leading_non_type_count = 0;
                    while (leading_non_type_count < template_params.size() &&
                           template_params[leading_non_type_count].is_non_type) {
                        leading_non_type_count++;
                    }
                    bool is_variadic_primary =
                        template_params.size() == leading_non_type_count + 1 &&
                        template_params.back().is_pack && !template_params.back().is_non_type;
                    if (is_variadic_primary) {
                        if (auto _rv = parse_variadic_primary_template_decl(program, is_exported, std::move(template_params)); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    } else {
                        if (auto _rv = parse_ordinary_class_template_forward_decl(program, is_exported, std::move(template_params)); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    }
                } else if (after_name == TokenKind::Less) {
                    std::string class_name = std::string(peek_at(name_offset).text.data(), peek_at(name_offset).text.size());
                    std::string qualified_class_name = qualify_name(class_name);
                    if (variadic_primary_template_params_.contains(qualified_class_name)) {
                        if (auto _rv = parse_variadic_specialization(program, is_exported); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    } else {
                        auto template_params_result = parse_generic_type_header();
                        if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
                        std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
                        if (auto _rv = parse_ordinary_class_partial_specialization(program, is_exported, std::move(template_params)); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    }
                } else {
                    auto template_params_result = parse_generic_type_header();
                    if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
                    std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
                    if (auto _rv = parse_class_def(program, is_exported, std::move(template_params)); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                }
            } else if (after_header_kind == TokenKind::KwStruct) {
                if (auto _rv = reject_unsafe_if_requested("a 'struct' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                // ch05 §5.14: a variadic generic type is class-only --
                // the only way to vary a type's own layout by arity is
                // recursive inheritance (real C++ has no syntax to
                // expand a pack directly into a member list), and a
                // struct has no inheritance at all (ch04 §4.1). A
                // struct name immediately followed by `;` or `<` would
                // only ever be one of those two variadic shapes, so
                // reject with a precise diagnostic rather than a
                // confusing downstream parse error.
                if (peek_at(after_header + 2).kind == TokenKind::Semicolon ||
                    peek_at(after_header + 2).kind == TokenKind::Less) {
                    const Token& tok = peek_at(after_header);
                    {
                        std::string _msg_4367{"a variadic generic type (parameter packs, ch05 §5.14) is only "};
                        _msg_4367 += "supported for 'class', never 'struct' -- building one needs recursive ";
                        _msg_4367 += "inheritance, which a struct doesn't have";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_4367));
                    }
                }
                SourceLocation loc = current_loc();
                auto template_params_result = parse_generic_type_header();
                if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
                std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
                auto generic_struct_def_result = parse_struct_def(program, is_exported, std::move(template_params));
                if (!generic_struct_def_result.has_value()) return std::unexpected(std::move(generic_struct_def_result).error());
                StructDef generic_struct_def = std::move(generic_struct_def_result).value();
                {
                    std::string _msg_4378{"struct '"};
                    _msg_4378 += generic_struct_def.name;
                    _msg_4378 += "'";
                    if (auto _rv = check_export_context(program, is_exported, generic_struct_def.namespace_path, loc, _msg_4378); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                }
                program.structs.push_back(std::move(generic_struct_def));
            } else if (after_header_kind == TokenKind::KwUnion) {
                if (auto _rv = reject_unsafe_if_requested("a 'union' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_alignment_specifiers(leading_alignments, "a generic 'union' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                const Token& tok = peek_at(after_header);
                return std::unexpected(ParseError(tok.line, tok.column,
                                  "generic unions are not supported in this version"));
            } else if (after_header_kind == TokenKind::KwConcept) {
                if (auto _rv = reject_unsafe_if_requested("a 'concept' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_alignment_specifiers(leading_alignments, "a 'concept' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "a 'concept' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = parse_concept_def(program, is_exported); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            } else {
                if (auto _rv = reject_alignment_specifiers(leading_alignments, "a function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "a function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                // ch05 §5.11: neither `class`/`struct` (a generic type,
                // handled above) nor `concept` -- the only remaining
                // legal shape is a full-header-form generic *function*
                // (`template<...> ReturnType name(params) { body }`,
                // ch05 §5.11's "generic functions may be spelled with
                // either the abbreviated or full header form").
                if (auto _rv = parse_generic_function_def(program, is_exported, requested_unsafe, requested_nodiscard,
                                           requested_nodiscard_reason);
                    !_rv.has_value()) {
                    return std::unexpected(std::move(_rv).error());
                }
            }
        } else {
            if (auto _rv = reject_packed_attribute(leading_attrs, attr_start_tok, "a function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (looks_like_type_start()) {
                std::size_t saved_pos = pos_;
                bool parsed_as_var_decl = false;
                // ch05: this whole "attempt to parse as a global variable
                // declaration" block is a backtracking attempt -- ANY
                // failure here (an ineligible `[[scpp::unsafe]]`, a
                // malformed declarator, or a rejected export context) means
                // this isn't a variable declaration after all, so we must
                // fall through and retry as a function declaration instead
                // of propagating the error outward.
                auto unsafe_check = reject_unsafe_if_requested("a variable declaration");
                if (unsafe_check.has_value()) {
                    // A speculative copy, not a move: if this whole
                    // attempt fails (or the export-context check just
                    // below it fails) we fall through past `pos_ =
                    // saved_pos` to retry as a function declaration
                    // instead, and that retry needs `leading_alignments`
                    // to still be intact -- unlike `pos_`, it has no
                    // "saved" counterpart to restore from once moved
                    // away.
                    std::vector<AlignmentSpecifier> leading_alignments_for_var_decl = leading_alignments;
                    auto decl_result = parse_global_var_decl(std::move(leading_alignments_for_var_decl));
                    if (decl_result.has_value()) {
                        StmtPtr decl = std::move(decl_result).value();
                        GlobalVar global{};

                        global.decl = std::move(decl);
                        global.namespace_path = namespace_stack_;
                        global.is_exported = is_exported;
                        SourceLocation loc = global.decl->loc;
                        std::string _msg_4430{"variable '"};
                        _msg_4430 += global.decl->var_name;
                        _msg_4430 += "'";
                        auto export_check = check_export_context(program, is_exported, global.namespace_path, loc,
                                               _msg_4430);
                        if (export_check.has_value()) {
                            program.globals.push_back(std::move(global));
                            parsed_as_var_decl = true;
                        }
                    }
                }
                if (parsed_as_var_decl) return {};
                pos_ = saved_pos;
            }
            if (auto _rv = reject_alignment_specifiers(leading_alignments, "a function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (auto _rv = parse_top_level_function_or_extern_group(program, is_exported, requested_unsafe, requested_nodiscard,
                                                     requested_nodiscard_reason);
                !_rv.has_value()) {
                return std::unexpected(std::move(_rv).error());
            }
        }
        return {};
    }

    // ch11 §11.4: `namespace a::b::c { ... }`, including the C++17
    // one-line nested form (all of `a::b::c` in one declaration) --
    // pushes every segment onto namespace_stack_, parses a nested
    // sequence of top-level items (recursively allowing further nested
    // namespace blocks), then pops them back off. When
    // `export_contents=true`, this is the sugar form `export namespace
    // ... { ... }`: every direct top-level item inside the block behaves
    // as though it had its own leading `export`.
    [[nodiscard]] std::expected<void, ParseError> parse_namespace_block(Program& program, bool export_contents = false) {
        nesting_depth_++;
        if (nesting_depth_ > kMaxNestingDepth) {
            nesting_depth_--;
            return std::unexpected(nesting_too_deep_error("namespace"));
        }
        auto result = parse_namespace_block_inner(program, export_contents);
        nesting_depth_--;
        return result;
    }

    [[nodiscard]] std::expected<void, ParseError> parse_namespace_block_inner(Program& program, bool export_contents) {
        if (auto _r = expect(TokenKind::KwNamespace, "'namespace'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::size_t pushed = 0;
        for (;;) {
            auto segment_result = expect(TokenKind::Identifier, "namespace name");
            if (!segment_result.has_value()) return std::unexpected(std::move(segment_result).error());
            std::string segment{segment_result.value().text.data(), segment_result.value().text.size()};
            namespace_stack_.push_back(std::move(segment));
            pushed++;
            if (!match(TokenKind::ColonColon)) break;
        }
        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (export_contents) {
            while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                if (auto _rv = parse_exported_top_level_entry(program, /*inherited_export=*/true); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
        } else {
            if (auto _rv = parse_top_level_items(program, /*inside_namespace=*/true); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        for (std::size_t i = 0; i < pushed; i++) namespace_stack_.pop_back();
        return {};
    }

    // Parses one top-level item that isn't a `struct`: an ordinary
    // function, or an `extern "C"` declaration/definition/block (ch02
    // §2.1), or a bare `extern` module-linkage declaration (ch11 §11.6).
    // Every function is checked by default (ch01) -- there is no
    // per-function keyword to consume here at all:
    //   <ret> <name>(<params>) { <body> }                          -- an
    //     ordinary definition (is_extern_c=false).
    //   extern "C" <ret> <name>(<params>) (';' | '{' ... '}')
    //     -- a single extern "C" item: a bodyless declaration (';',
    //     always implicitly unchecked -- calling it needs `unsafe { }`,
    //     ch02 §2.1) or a definition (checked like any other function).
    //   extern "C" { <item> <item> ... }                          -- block
    //     sugar for repeating `extern "C"` on each nested item, matching
    //     real C++.
    //   extern <ret> <name>(<params>);                             -- a
    //     bare (non-"C") extern declaration (ch11 §11.6): ordinary scpp
    //     linkage, no block-sugar form (that's an extern-"C"-only
    //     convenience for repeating a linkage string).
    // `is_exported` (ch11 §11.3) is only meaningful for the ordinary/bare-
    // extern cases: an `extern "C"` declaration is never namespace-
    // qualified or mangled (its name must stay the real, plain C symbol
    // regardless of enclosing namespace -- see qualify_name), so an
    // `export` marker on one is simply not applicable and is ignored.
    [[nodiscard]] std::expected<void, ParseError> parse_top_level_function_or_extern_group(Program& program, bool is_exported, bool is_unsafe = false,
                                                  bool is_nodiscard = false,
                                                  const std::string& nodiscard_reason = {}) {
        SourceLocation loc = current_loc();
        while (match(TokenKind::KwInline)) {
        }
        if (match(TokenKind::KwExtern)) {
            if (!check(TokenKind::StringLiteral)) {
                // Bare extern (ch11 §11.6): always a single bodyless
                // declaration, ordinary scpp linkage.
                auto fn_result =
                    parse_function(/*is_extern_c=*/false, /*is_module_extern=*/true, is_unsafe, is_nodiscard,
                                   nodiscard_reason);
                if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
                Function fn = std::move(fn_result).value();
                fn.loc = loc;
                fn.name = qualify_name(fn.name);
                fn.namespace_path = namespace_stack_;
                fn.is_exported = is_exported;
                {
                    std::string _msg_4524{"function '"};
                    _msg_4524 += fn.name;
                    _msg_4524 += "'";
                    if (auto _rv = check_export_context(program, is_exported, fn.namespace_path, loc, _msg_4524); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                }
                program.functions.push_back(std::move(fn));
                return {};
            }
            if (auto _rv = parse_c_linkage_string(); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (check(TokenKind::LBrace)) {
                advance(); // '{'
                while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                    if (check(TokenKind::KwStruct)) {
                        auto struct_result = parse_struct_def(program, /*is_exported=*/false);
                        if (!struct_result.has_value()) return std::unexpected(std::move(struct_result).error());
                        StructDef __struct_result_value = std::move(struct_result).value();
                        program.structs.push_back(std::move(__struct_result_value));
                        continue;
                    }
                    if (check(TokenKind::KwUnion)) {
                        auto union_result = parse_union_def();
                        if (!union_result.has_value()) return std::unexpected(std::move(union_result).error());
                        StructDef __union_result_value = std::move(union_result).value();
                        program.structs.push_back(std::move(__union_result_value));
                        continue;
                    }
                    SourceLocation item_loc = current_loc();
                    auto item_fn_result = parse_function(/*is_extern_c=*/true);
                    if (!item_fn_result.has_value()) return std::unexpected(std::move(item_fn_result).error());
                    Function item_fn = std::move(item_fn_result).value();
                    item_fn.loc = item_loc;
                    program.functions.push_back(std::move(item_fn));
                }
                if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return {};
            }
            auto fn_result =
                parse_function(/*is_extern_c=*/true, /*is_module_extern=*/false, is_unsafe, is_nodiscard,
                               nodiscard_reason);
            if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
            Function fn = std::move(fn_result).value();
            fn.loc = loc;
            program.functions.push_back(std::move(fn));
            return {};
        }
        auto out_of_line_result = parse_out_of_line_member_definition(program, loc, is_unsafe, is_nodiscard, nodiscard_reason);
        if (!out_of_line_result.has_value()) return std::unexpected(std::move(out_of_line_result).error());
        if (out_of_line_result.value()) {
            return {};
        }
        auto fn_result =
            parse_function(/*is_extern_c=*/false, /*is_module_extern=*/false, is_unsafe, is_nodiscard,
                           nodiscard_reason);
        if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
        Function fn = std::move(fn_result).value();
        fn.loc = loc;
        fn.name = qualify_name(fn.name);
        fn.namespace_path = namespace_stack_;
        fn.is_exported = is_exported;
        {
            std::string _msg_4577{"function '"};
            _msg_4577 += fn.name;
            _msg_4577 += "'";
            if (auto _rv = check_export_context(program, is_exported, fn.namespace_path, loc, _msg_4577); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        program.functions.push_back(std::move(fn));
        return {};
    }

    // Consumes and validates the linkage string literal after `extern`.
    // v0.1 only accepts the literal "C" (not "C++" or anything else) --
    // see ch02 §2.1.
    [[nodiscard]] std::expected<void, ParseError> parse_c_linkage_string() {
        auto tok_result = expect(TokenKind::StringLiteral, "a linkage string (e.g. \"C\")");
        if (!tok_result.has_value()) return std::unexpected(std::move(tok_result).error());
        const Token& tok = std::move(tok_result).value();
        // The linkage is a *string-literal*, so adjacent literals are
        // concatenated first ([lex.string]/1) and the result is then
        // checked -- `extern "C" "C"` is `extern "CC"`, an unsupported
        // linkage, rather than a confusing "expected a type name" at the
        // second literal.
        auto linkage_result = decode_adjacent_string_literals(tok);
        if (!linkage_result.has_value()) return std::unexpected(std::move(linkage_result).error());
        std::string linkage = std::move(linkage_result).value();
        if (linkage != "C") {
            {
                std::string _msg_4593{"unsupported linkage \""};
                _msg_4593 += linkage;
                _msg_4593 += "\": only extern \"C\" is supported in this version";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_4593));
            }
        }
        return {};
    }

    // Decodes a StringLiteral token's text (e.g. "a\nb") into its byte
    // content. `tok.text` includes the surrounding double quotes (see
    // StringLiteral's definition in lexer.cppm). Supports the same
    // minimal named-escape set as decode_char_literal above: \n \t \r \\
    // \' \" \0 -- no hex/octal escapes. Unlike a char literal, any number
    // of characters (including zero -- an empty string "") is valid.
    [[nodiscard]] std::expected<std::string, ParseError> decode_string_literal(const Token& tok) {
        if (tok.text.size() < 2) {
            {
                std::string _msg_4607{"unterminated string literal "};
                _msg_4607 += std::string(tok.text.data(), tok.text.size());
                return std::unexpected(ParseError(tok.line, tok.column, _msg_4607));
            }
        }
        std::string_view inner = tok.text.substr(static_cast<std::size_t>(1), tok.text.size() - 2);
        std::string result{};

        result.reserve(inner.size());
        for (std::size_t i = 0; i < inner.size(); i++) {
            if (inner.at(i) != '\\') {
                result.push_back(inner.at(i));
                continue;
            }
            if (i + 1 >= inner.size()) {
                {
                    std::string _msg_4620{"invalid string literal "};
                    _msg_4620 += std::string(tok.text.data(), tok.text.size());
                    _msg_4620 += ": trailing '\\' with no following escape character";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_4620));
                }
            }
            i++;
            switch (inner.at(i)) {
                case 'n': result.push_back('\n'); break;
                case 't': result.push_back('\t'); break;
                case 'r': result.push_back('\r'); break;
                case '0': result.push_back('\0'); break;
                case '\\': result.push_back('\\'); break;
                case '\'': result.push_back('\''); break;
                case '"': result.push_back('"'); break;
                default:
                    return std::unexpected(ParseError(tok.line, tok.column,
                                      [&]() -> std::string {
                        std::string _msg_4634{"invalid string literal "};
                        _msg_4634 += std::string(tok.text.data(), tok.text.size());
                        _msg_4634 += ": unsupported escape sequence '\\";
                        _msg_4634.push_back(inner.at(i));
                        _msg_4634 += "' (supported: \\n \\t \\r \\\\ \\' \\\" \\0)";
                        return _msg_4634;
                    }()));
            }
        }
        return std::move(result);
    }

    // [lex.string]/1: adjacent string-literal tokens are concatenated.
    // The spec adopts the C++ standard's lexical rules unchanged (front
    // matter §2, and §4(1)'s "the C++ standard as modified by this
    // document"); nothing in it modifies [lex.string], so `"a" "b"` is
    // one string literal spelling `ab`, exactly as in C++.
    //
    // The standard calls this translation phase 6, i.e. lexical, but the
    // lexer here deliberately does not decode literals -- it records only
    // a token's extent, and `Token::text` is a view into the source
    // (lexer.cppm's StringLiteral). Splicing raw *source* text would be
    // wrong in general: an escape sequence must be decoded within its own
    // literal before the pieces are joined, or `"\x41" "B"` would decode
    // as the single escape `\x41B`. So the join happens where token text
    // becomes byte content, which is here; the result is what phase 6
    // specifies.
    [[nodiscard]] std::expected<std::string, ParseError> decode_adjacent_string_literals(const Token& first) {
        auto first_result = decode_string_literal(first);
        if (!first_result.has_value()) return std::unexpected(std::move(first_result).error());
        std::string result = std::move(first_result).value();
        while (check(TokenKind::StringLiteral)) {
            const Token& next = advance();
            auto next_result = decode_string_literal(next);
            if (!next_result.has_value()) return std::unexpected(std::move(next_result).error());
            result += std::move(next_result).value();
        }
        return result;
    }

    // Decodes a CharLiteral token's text (e.g. 'a', '\n', '\\', '\'', '\0')
    // into its ordinal value. `tok.text` includes the surrounding single
    // quotes (see CharLiteral's definition in lexer.cppm). Supports the
    // same minimal named-escape set as decode_string_literal above: \n \t
    // \r \\ \' \" \0 -- no hex/octal escapes.
    [[nodiscard]] std::expected<std::int64_t, ParseError> decode_char_literal(const Token& tok) {
        // A well-formed literal is always at least `''` (2 quote chars);
        // anything shorter means the lexer hit EOF before a closing
        // quote (an unterminated literal) -- guard before the substr
        // below so that case reports a clear error instead of
        // underflowing `tok.text.size() - 2`.
        if (tok.text.size() < 2) {
            {
                std::string _msg_4655{"unterminated char literal "};
                _msg_4655 += std::string(tok.text.data(), tok.text.size());
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_4655));
            }
        }
        std::string_view inner = tok.text.substr(static_cast<std::size_t>(1), tok.text.size() - 2);
        if (inner.size() == 1 && inner.at(0) != '\\') {
            std::uint8_t plain_char_byte = static_cast<std::uint8_t>(inner.at(0));
            std::int64_t plain_char_value = static_cast<std::int64_t>(plain_char_byte);
            return plain_char_value;
        }
        if (inner.size() == 2 && inner.at(0) == '\\') {
            std::int64_t escaped_value = 0;
            switch (inner.at(1)) {
                case 'n': escaped_value = static_cast<std::int64_t>('\n'); return escaped_value;
                case 't': escaped_value = static_cast<std::int64_t>('\t'); return escaped_value;
                case 'r': escaped_value = static_cast<std::int64_t>('\r'); return escaped_value;
                case '0': escaped_value = static_cast<std::int64_t>('\0'); return escaped_value;
                case '\\': escaped_value = static_cast<std::int64_t>('\\'); return escaped_value;
                case '\'': escaped_value = static_cast<std::int64_t>('\''); return escaped_value;
                case '"': escaped_value = static_cast<std::int64_t>('"'); return escaped_value;
                default: break;
            }
        }
        {
            std::string _msg_4674{"invalid char literal "};
            _msg_4674 += std::string(tok.text.data(), tok.text.size());
            _msg_4674 += ": must be exactly one character or one of the supported escape ";
            _msg_4674 += "sequences (\\n \\t \\r \\\\ \\' \\\" \\0)";
            return std::unexpected(ParseError(tok.line, tok.column,
                          _msg_4674));
        }
    }

    // An enum's underlying type is exactly the integral scalars: not
    // `bool` (an enum needs more than two values) and not the floating
    // ones (an enumerator is an integer).
    [[nodiscard]] bool is_valid_enum_underlying_type(const Type& type) const {
        std::string_view name{type.name};
        return type.kind == TypeKind::Named && scpp::is_integral_scalar_type_name(name);
    }

    // The recursive helper behind parse_enum_constant_expr, below --
    // a plain recursive member function rather than a self-referencing
    // std::function-wrapped lambda (scpp's std::function model has no
    // default constructor for this instantiation, and a directly self-
    // initializing lambda capture isn't valid either: see this
    // function's own call site for the full reasoning) -- functionally
    // identical, just avoiding both of those.
    [[nodiscard]] std::expected<std::int64_t, ParseError> eval_enum_constant_expr(const Expr& current,
                                                                                  const std::string& enum_name) {
        switch (current.kind) {
            case ExprKind::IntegerLiteral:
            case ExprKind::CharLiteral:
                // std::int64_t all the way through: Expr::int_value and
                // the EnumVariant::value this eventually lands in are
                // both std::int64_t (ast.cppm), so carrying the value in
                // that same named type avoids a conversion at every step
                // (ch06: no two differently-spelled scalar type names
                // are ever interchangeable, even when their underlying
                // representation is identical on every target scpp
                // supports today -- so a `long` return slot here would
                // need an explicit cast in *and* an explicit cast out).
                return current.int_value;
            case ExprKind::Unary:
                if (current.unary_op == UnaryOp::Neg && current.lhs != nullptr) {
                    auto inner_result = eval_enum_constant_expr(*current.lhs, enum_name);
                    if (!inner_result.has_value()) return std::unexpected(std::move(inner_result).error());
                    return -inner_result.value();
                }
                [[fallthrough]];
            default:
                return std::unexpected(ParseError(current.loc.line, current.loc.column,
                                 [&]() -> std::string {
                    std::string _msg_4707{"enum class '"};
                    _msg_4707 += enum_name;
                    _msg_4707 += "' only supports integer-literal enumerator values in this version";
                    return _msg_4707;
                }()));
        }
    }

    [[nodiscard]] std::expected<std::int64_t, ParseError> parse_enum_constant_expr(const std::string& enum_name) {
        auto expr_result = parse_unary();
        if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
        ExprPtr expr = std::move(expr_result).value();
        return eval_enum_constant_expr(*expr, enum_name);
    }

    [[nodiscard]] std::expected<EnumDef, ParseError> parse_enum_def() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwEnum, "'enum'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!match(TokenKind::KwClass)) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "only 'enum class' is supported in this version; old-style unscoped 'enum' is not supported"));
        }

        EnumDef def{};

        auto bare_name_result = expect(TokenKind::Identifier, "enum class name");
        if (!bare_name_result.has_value()) return std::unexpected(std::move(bare_name_result).error());
        std::string bare_name = std::string(bare_name_result.value().text.data(), bare_name_result.value().text.size());
        std::string nested_type_owner = take_pending_nested_type_owner();
        if (!nested_type_owner.empty()) {
            def.name = nested_type_owner;
            def.name += "::";
            def.name += bare_name;
            if (auto _rv = register_local_type_name(bare_name, def.name, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        } else {
            def.name = qualify_name(bare_name);
        }
        def.namespace_path = namespace_stack_;
        if (match(TokenKind::Colon)) {
            auto _tmp_result = parse_type();
            if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
            def.underlying_type = std::move(_tmp_result).value();
            if (!is_valid_enum_underlying_type(def.underlying_type)) {
                return std::unexpected(ParseError(loc.line, loc.column,
                                 "enum class underlying type must be an integral scalar type in this version"));
            }
        }
        struct_names_.insert(def.name);

        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::int64_t next_value = 0;
        bool first = true;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            if (!first) { if (auto _r = expect(TokenKind::Comma, "','"); !_r.has_value()) return std::unexpected(std::move(_r).error()); }
            if (check(TokenKind::RBrace)) break;
            first = false;

            EnumVariant variant{};

            auto variant_name_result = expect(TokenKind::Identifier, "enumerator name");
            if (!variant_name_result.has_value()) return std::unexpected(std::move(variant_name_result).error());
            variant.name = def.name;
            variant.name += "::";
            variant.name += std::string(variant_name_result.value().text.data(), variant_name_result.value().text.size());
            variant.value = next_value;
            if (match(TokenKind::Assign)) {
                auto value_result = parse_enum_constant_expr(def.name);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                variant.value = value_result.value();
            }
            def.variants.push_back(std::move(variant));
            next_value = def.variants.back().value + 1;
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return def;
    }

    // ch05 §5.14: `template_params`, non-empty exactly when the caller
    // (parse_top_level_item) already consumed a `template<...>` header
    // in front of `struct`, must always be concept-constrained (never
    // bare) -- unlike `class`, a struct's fields must *all* be trivial
    // (ch04 §4.1), and triviality is a whole-type layout/ABI property no
    // per-member clause could decompose (a struct has no methods to
    // decompose it across in the first place, unlike Function::
    // method_requires_concept). Otherwise behaves exactly like
    // parse_class_def's own generic handling: registers the type
    // parameter's own bare name as a temporary type name for the
    // duration of this one struct's body, removed again immediately
    // afterward.
    [[nodiscard]] std::expected<StructDef, ParseError> parse_struct_def(Program& program, bool is_exported, std::vector<GenericTypeParam> template_params = {},
                               std::vector<AlignmentSpecifier> leading_alignments = {},
                               std::optional<std::string> forced_qualified_name = {},
                               bool is_local_definition = false) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwStruct, "'struct'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        StructDef def{};

        def.loc = loc;
        // ch05 §5.15: `struct [[scpp::thread_movable]] Name { ... };` --
        // real C++ grammar already gives a class-head an optional
        // attribute-specifier-seq right after its class-key, the same
        // slot `struct [[deprecated]] Name { ... };` would use.
        auto attrs_result = parse_attribute_specifier_seq();
        if (!attrs_result.has_value()) return std::unexpected(std::move(attrs_result).error());
        ParsedAttributes attrs = std::move(attrs_result).value();
        auto trailing_alignments_result = parse_alignment_specifier_seq();
        if (!trailing_alignments_result.has_value()) return std::unexpected(std::move(trailing_alignments_result).error());
        std::vector<AlignmentSpecifier> trailing_alignments = std::move(trailing_alignments_result).value();
        def.thread_movable_override = attrs.has("thread_movable");
        def.thread_shareable_override = attrs.has("thread_shareable");
        if (attrs.has("interface")) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "a declaration introduced by 'struct' shall not be marked '[[scpp::interface]]' (spec §11.1(2.2))"));
        }
        def.is_packed = attrs.has("packed");
        def.alignment_specs = std::move(leading_alignments);
        // std::vector::insert(pos, first, last) isn't supported by scpp's
        // self-hosting compiler yet (nor is std::make_move_iterator), so
        // append trailing_alignments onto alignment_specs one element at a
        // time instead (AlignmentSpecifier is copy-constructible, so a
        // plain copy -- rather than a move, which scpp's move-checker
        // doesn't yet support for an indexed vector element access like
        // trailing_alignments.at(i) -- is fine here; this is a small,
        // one-time list built during parsing, not a hot path).
        for (std::size_t i = 0; i < trailing_alignments.size(); i++) {
            def.alignment_specs.push_back(trailing_alignments.at(i));
        }
        def.is_nodiscard = attrs.has_nodiscard;
        def.nodiscard_reason = attrs.nodiscard_reason;
        if (attrs.thread_movable_if_movable_expr != nullptr || attrs.thread_movable_if_shareable_expr != nullptr) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'[[scpp::thread_movable_if(a, b)]]' is only supported on class declarations"));
        }
        std::string bare_name{};

        {
            auto bare_name_result = expect(TokenKind::Identifier, "struct name");
            if (!bare_name_result.has_value()) return std::unexpected(std::move(bare_name_result).error());
            bare_name = std::string(bare_name_result.value().text.data(), bare_name_result.value().text.size());
        }
        if (check(TokenKind::KwAlignas)) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'alignas' must appear before a struct name, not after it (spec §9.3)"));
        }
        std::string nested_type_owner = take_pending_nested_type_owner();
        if (forced_qualified_name.has_value()) {
            def.name = *forced_qualified_name;
        } else if (!nested_type_owner.empty()) {
            def.name = nested_type_owner;
            def.name += "::";
            def.name += bare_name;
        } else if (is_local_definition) {
            def.name = fresh_local_type_name(bare_name);
        } else {
            def.name = qualify_name(bare_name);
        }
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported || exported_forward_struct_exists(program, def.name);
        if (auto _rv = register_record_tag_kind(def.name, RecordTagKind::Struct, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (is_local_definition || forced_qualified_name.has_value() || !nested_type_owner.empty()) { if (auto _rv = register_local_type_name(bare_name, def.name, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error()); }
        // Register the (fully-qualified) name before parsing the body so
        // a field can refer to the enclosing struct via a pointer (e.g.
        // `Node* next;`).
        struct_names_.insert(def.name);
        bool is_generic = !template_params.empty();
        if (is_generic) {
            bool saw_type = false;
            bool saw_non_type = false;
            for (const GenericTypeParam& param : template_params) {
                saw_type = saw_type || !param.is_non_type;
                saw_non_type = saw_non_type || param.is_non_type;
                if (!param.is_non_type && param.concept_name.empty()) {
                    const Token& tok = peek();
                    {
                        std::string _msg_4850{"a generic struct's own type parameter '"};
                        _msg_4850 += param.name;
                        _msg_4850 += "' cannot be bare -- struct field triviality (ch04 §4.1) is a ";
                        _msg_4850 += "whole-type property, so it must be constrained by a concept at the ";
                        _msg_4850 += "struct itself (ch05 §5.14): write 'template<Concept ";
                        _msg_4850 += param.name;
                        _msg_4850 += "> struct ";
                        _msg_4850 += bare_name;
                        _msg_4850 += "' instead, or use 'class' if per-method constraints are enough";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_4850));
                    }
                }
                if (!param.is_non_type) {
                    struct_names_.insert(param.name);
                    class_names_.insert(param.name);
                }
            }
            if (saw_type && saw_non_type) {
                const Token& tok = peek();
                {
                    std::string _msg_4865{"ordinary generic structs cannot yet mix type and non-type template "};
                    _msg_4865 += "parameters in one parameter list";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_4865));
                }
            }
            generic_type_names_.insert(def.name);
            ordinary_generic_type_template_params_.insert_or_assign(def.name, template_params);
        }
        def.template_params = template_params;
        if (is_generic) def.template_owner_id = next_generic_template_owner_id();

        const Token& maybe_base_clause_tok = peek();
        if (match(TokenKind::Colon)) {
            const Token& tok = maybe_base_clause_tok;
            return std::unexpected(ParseError(tok.line, tok.column,
                             "a declaration introduced by 'struct' shall not have a base-clause (spec §11.1(2.1))"));
        }

        if (match(TokenKind::Semicolon)) {
            if (is_generic) {
                {
                    std::string _msg_4884{"an ordinary bodyless forward declaration is only supported for a non-generic "};
                    _msg_4884 += "'struct' in this version";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_4884));
                }
            }
            if (def.is_packed || !def.alignment_specs.empty() || def.thread_movable_override || def.thread_shareable_override ||
                def.is_nodiscard) {
                {
                    std::string _msg_4890{"scpp struct layout/thread/nodiscard attributes are not supported on a bodyless "};
                    _msg_4890 += "forward declaration";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_4890));
                }
            }
            def.is_forward_declaration = true;
            return def;
        }

        {
            std::string _msg_4902{"a declaration introduced by 'struct' shall not declare a virtual member function or virtual destructor "};
            _msg_4902 += "(spec §11.1(2.3))";
            // Wrap each lambda literal in an explicit, fully-qualified
            // std::function<Sig>(...) constructor-call at the argument
            // position itself, rather than either (a) passing the bare
            // lambda literal directly as a call argument, or (b) first
            // binding it to a named RecordUsingHandlerFn/
            // RecordFieldAdderFn local variable. (a) fails codegen's
            // overload resolution (resolve_overload_by_type in
            // src/compiler/codegen/semantics.cppm): the implicit
            // lambda-to-std::function converting constructor is only
            // instantiated/resolved when the enclosing expression is
            // itself recognized as a constructor call (movecheck's
            // maybe_instantiate_generic_constructor_overloads, in
            // src/compiler/movecheck/monomorphize.cppm, triggers only
            // for `New` and bare-Call constructor-call expressions --
            // never for a bare lambda-literal argument). (b) resolves
            // the codegen gap but introduces a *different*, unrelated
            // borrow-check conflict: naming the closure as a local
            // extends its (this-capturing) borrow's lifetime to
            // overlap with parse_record_body_into's own implicit
            // `this` receiver borrow at the call below ("cannot use
            // 'this' while it is mutably borrowed"). An explicit,
            // fully-qualified constructor-call expression used
            // directly as the argument sidesteps both: it *is* a
            // recognized constructor-call site (so the F-specific
            // clone gets instantiated, same as `New`), yet remains a
            // transient temporary like the original bare literal (no
            // named local, so no extended this-borrow). Using the
            // RecordUsingHandlerFn/RecordFieldAdderFn *alias* name
            // instead of the fully-qualified std::function<Sig> name
            // here does *not* work ("cannot deduce template arguments
            // for generic type 'std::function' from this constructor
            // call") -- the alias must be spelled out.
            if (auto _rv = parse_record_body_into(
                program, bare_name, def.name, def.name, def.template_params, def.is_exported, def.template_owner_id,
                AccessSpecifier::Public,
                /*allow_using_declarations=*/false, /*allow_virtual_members=*/false, "struct",
                "a struct member declaration",
                _msg_4902,
                std::function<bool(AccessSpecifier)>([](AccessSpecifier access [[maybe_unused]]) -> bool { return true; }),
                // By-const-ref parameters (matching RecordFieldAdderFn's
                // own by-reference contract, see its declaration's
                // comment above) mean this copies field_type/field_name/
                // default_initializer/alignment_specs into the new
                // StructField rather than moving them.
                std::function<void(const SourceLocation&, const Type&, const std::string&, AccessSpecifier,
                                    const std::optional<Initializer>&, const std::vector<AlignmentSpecifier>&)>(
                    [&](const SourceLocation& field_loc, const Type& field_type, const std::string& field_name, AccessSpecifier access,
                        const std::optional<Initializer>& default_initializer, const std::vector<AlignmentSpecifier>& alignment_specs) {
                        StructField field{};

                        field.loc = field_loc;
                        field.type = field_type;
                        field.name = field_name;
                        field.access = access;
                        field.default_initializer = default_initializer;
                        field.alignment_specs = alignment_specs;
                        def.fields.push_back(std::move(field));
                    }));
            !_rv.has_value()) {
            return std::unexpected(std::move(_rv).error());
        }


        if (is_generic) {
            for (const GenericTypeParam& param : template_params) {
                if (!param.is_non_type) {
                    struct_names_.erase(param.name);
                    class_names_.erase(param.name);
                }
            }
        }
        return def;
        }
    }

    [[nodiscard]] std::expected<StructDef, ParseError> parse_union_def(std::vector<AlignmentSpecifier> leading_alignments = {}) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwUnion, "'union'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        StructDef def{};

        def.loc = loc;
        def.is_union = true;
        auto attrs_result = parse_attribute_specifier_seq();
        if (!attrs_result.has_value()) return std::unexpected(std::move(attrs_result).error());
        ParsedAttributes attrs = std::move(attrs_result).value();
        auto trailing_alignments_result = parse_alignment_specifier_seq();
        if (!trailing_alignments_result.has_value()) return std::unexpected(std::move(trailing_alignments_result).error());
        std::vector<AlignmentSpecifier> trailing_alignments = std::move(trailing_alignments_result).value();
        def.thread_movable_override = attrs.has("thread_movable");
        def.thread_shareable_override = attrs.has("thread_shareable");
        def.is_packed = attrs.has("packed");
        def.alignment_specs = std::move(leading_alignments);
        // std::vector::insert(pos, first, last) isn't supported by scpp's
        // self-hosting compiler yet (nor is std::make_move_iterator), so
        // append trailing_alignments onto alignment_specs one element at a
        // time instead (AlignmentSpecifier is copy-constructible, so a
        // plain copy -- rather than a move, which scpp's move-checker
        // doesn't yet support for an indexed vector element access like
        // trailing_alignments.at(i) -- is fine here; this is a small,
        // one-time list built during parsing, not a hot path).
        for (std::size_t i = 0; i < trailing_alignments.size(); i++) {
            def.alignment_specs.push_back(trailing_alignments.at(i));
        }
        def.is_nodiscard = attrs.has_nodiscard;
        def.nodiscard_reason = attrs.nodiscard_reason;
        if (attrs.thread_movable_if_movable_expr != nullptr || attrs.thread_movable_if_shareable_expr != nullptr) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'[[scpp::thread_movable_if(a, b)]]' is only supported on class declarations"));
        }
        auto bare_name_result = expect(TokenKind::Identifier, "union name");
        if (!bare_name_result.has_value()) return std::unexpected(std::move(bare_name_result).error());
        std::string bare_name = std::string(bare_name_result.value().text.data(), bare_name_result.value().text.size());
        if (check(TokenKind::KwAlignas)) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'alignas' must appear before a union name, not after it (spec §9.3)"));
        }
        std::string nested_union_owner = take_pending_nested_type_owner();
        if (!nested_union_owner.empty()) {
            def.name = nested_union_owner;
            def.name += "::";
            def.name += bare_name;
            if (auto _rv = register_local_type_name(bare_name, def.name, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        } else {
            def.name = qualify_name(bare_name);
        }
        def.namespace_path = namespace_stack_;
        if (auto _rv = register_record_tag_kind(def.name, RecordTagKind::Union, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        struct_names_.insert(def.name);

        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        bool saw_field = false;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            StructField field{};

            field.loc = current_loc();
            auto field_alignment_specs_result = parse_alignment_specifier_seq();
            if (!field_alignment_specs_result.has_value()) return std::unexpected(std::move(field_alignment_specs_result).error());
            field.alignment_specs = std::move(field_alignment_specs_result).value();
            auto base_result = parse_type();
            if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
            Type base = std::move(base_result).value();
            if (starts_function_pointer_declarator()) {
                auto field_fn_ptr_type_result = parse_function_pointer_declarator(std::move(base), field.name);
                if (!field_fn_ptr_type_result.has_value()) return std::unexpected(std::move(field_fn_ptr_type_result).error());
                field.type = std::move(field_fn_ptr_type_result).value();
                if (check(TokenKind::Colon)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column, "bit-field declarations are not supported in this version"));
                }
                auto default_init_result = parse_optional_default_initializer("a union member declaration");
                if (!default_init_result.has_value()) return std::unexpected(std::move(default_init_result).error());
                field.default_initializer = std::move(default_init_result).value();
            } else {
                auto field_name_result = expect(TokenKind::Identifier, "field name");
                if (!field_name_result.has_value()) return std::unexpected(std::move(field_name_result).error());
                field.name = std::string(field_name_result.value().text.data(), field_name_result.value().text.size());
                if (check(TokenKind::Colon)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column, "bit-field declarations are not supported in this version"));
                }
                auto field_array_type_result = parse_array_suffix(base);
                if (!field_array_type_result.has_value()) return std::unexpected(std::move(field_array_type_result).error());
                field.type = std::move(field_array_type_result).value();
                auto default_init_result2 = parse_optional_default_initializer("a union member declaration");
                if (!default_init_result2.has_value()) return std::unexpected(std::move(default_init_result2).error());
                field.default_initializer = std::move(default_init_result2).value();
            }
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            def.fields.push_back(std::move(field));
            saw_field = true;
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!saw_field) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "a union must declare at least one field"));
        }
        return def;
    }

    // Parses a parenthesized, comma-separated parameter list `(<type>
    // <name>, ...)`, including the enclosing parens -- shared by every
    // class member function (method/constructor; a destructor is always
    // zero-arg, parsed directly). Deliberately separate from
    // parse_function's own inline version (which also handles extern
    // "C"'s trailing `...`, never relevant to a method/constructor): the
    // two are simple and small enough that duplicating this one loop
    // body is lower-risk than threading varargs-specific logic through a
    // shared helper.
    static constexpr const char* kUnnamedDefaultedSingleParam() { return "__defaulted_single_param"; }

    [[nodiscard]] bool has_unnamed_defaulted_single_param(const std::vector<Param>& params) const {
        return params.size() == 1 && params[0].name == kUnnamedDefaultedSingleParam();
    }

    [[nodiscard]] std::expected<std::vector<Param>, ParseError> parse_param_list(bool allow_unnamed_single_parameter = false) {
        std::vector<Param> params{};

        bool saw_default_argument = false;
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!check(TokenKind::RParen)) {
            while (true) {
                Param param{};
                param.loc = SourceLocation{peek().line, peek().column};

                Type base_type{};

                {
                    auto base_type_result = parse_param_type_with_lifetime_attributes_enabled(param.generic_concept);
                    if (!base_type_result.has_value()) return std::unexpected(std::move(base_type_result).error());
                    base_type = std::move(base_type_result).value();
                }
                param.is_parameter_pack = match(TokenKind::Ellipsis);
                if (param.is_parameter_pack && param.generic_concept.empty() &&
                    !referenced_pack_type_param_name(base_type).has_value()) {
                    const Token& tok = peek();
                    {
                        std::string _msg_5062{"parameter packs are only supported for the abbreviated generic form "};
                        _msg_5062 += "('Concept auto&... args') in this version (ch05 §5.11)";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_5062));
                    }
                }
                Type param_type{};

                if (allow_unnamed_single_parameter && params.empty() && !param.is_parameter_pack && check(TokenKind::RParen)) {
                    param.name = std::string(kUnnamedDefaultedSingleParam());
                    param_type = std::move(base_type);
                } else {
                    auto param_type_result = parse_named_declarator(std::move(base_type), param.name, "parameter name");
                    if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                    param_type = std::move(param_type_result).value();
                }
                if (param.is_parameter_pack && !check(TokenKind::RParen)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column,
                                      "a parameter pack must be the last parameter in the list (ch05 §5.11)"));
                }
                set_param_declared_type(param, std::move(param_type));
                // ch05 §5.15: `T&& f [[scpp::thread_movable]]` -- a
                // trailing attribute-specifier-seq right after a
                // parameter's own declarator (real C++ grammar already
                // gives a parameter-declaration one, the same slot
                // `int x [[maybe_unused]]` would use), constraining this
                // parameter's (possibly template-deduced) type to
                // satisfy the corresponding thread-safety property --
                // checked at each call site (see the Monomorphizer's own
                // check_thread_safety_constraint), only meaningful when
                // this parameter's own type actually depends on one of
                // the enclosing function's own template parameters.
                const Token& param_attr_start_tok = peek();
                auto param_attrs_result = parse_attribute_specifier_seq();
                if (!param_attrs_result.has_value()) return std::unexpected(std::move(param_attrs_result).error());
                ParsedAttributes param_attrs = std::move(param_attrs_result).value();
                if (auto _rv = reject_packed_attribute(param_attrs, param_attr_start_tok, "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                param.require_thread_movable = param_attrs.has("thread_movable");
                param.require_thread_shareable = param_attrs.has("thread_shareable");
                if (auto _rv = merge_lifetime_attribute(param.lifetime, param_attrs.lifetime, param_attr_start_tok,
                                         "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                LifetimeAnnotation param_lifetime_for_hoist = param.lifetime;
                if (auto _rv = hoist_type_lifetime_annotation(param.type, param_lifetime_for_hoist, param_attr_start_tok, "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                param.lifetime = std::move(param_lifetime_for_hoist);
                if (match(TokenKind::Assign)) {
                    if (param.is_parameter_pack) {
                        const Token& tok = peek();
                        return std::unexpected(ParseError(tok.line, tok.column, "a parameter pack cannot have a default argument"));
                    }
                    auto default_expr_result = parse_default_argument_expr(param.type);
                    if (!default_expr_result.has_value()) return std::unexpected(std::move(default_expr_result).error());
                    param.default_expr = std::shared_ptr<Expr>(std::move(default_expr_result).value().release());
                    saw_default_argument = true;
                } else if (saw_default_argument) {
                    const Token& tok = peek();
                    {
                        std::string _msg_5114{"once a parameter has a default argument, every later parameter must also "};
                        _msg_5114 += "have one";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                     _msg_5114));
                    }
                }
                params.push_back(std::move(param));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return params;
    }

    [[nodiscard]] std::expected<void, ParseError> reject_unnamed_defaulted_single_param_if_needed(const std::vector<Param>& params,
                                                         const Function& fn,
                                                         const SourceLocation& loc) const {
        if (has_unnamed_defaulted_single_param(params) && !fn.is_defaulted) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "an unnamed parameter is only supported in a '= default' declaration here"));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> validate_defaulted_special_member(const Function& fn, const SourceLocation& loc) const {
        if (!fn.is_defaulted) return {};
        if (is_destructor_function(fn) || is_defaulted_special_member_equivalent_to_implicit_omission(fn)) return {};
        if (is_defaulted_equality_operator_function(fn)) return {};
        {
            std::string _msg_5140{"only a destructor, default constructor, copy/move constructor, copy/move assignment operator, "};
            _msg_5140 += "or equality operator may be declared '= default' in this version";
            return std::unexpected(ParseError(loc.line, loc.column,
                         _msg_5140));
        }
    }

    // A parameter's own `= {...}` default is a context-typed brace
    // position exactly like every other one (see
    // parse_brace_initializer_element), the context being the
    // parameter's declared type -- so it produces the same
    // BracedInitList the other positions do, and is bound to that type
    // by whichever call site the default is cloned into. It used to
    // fabricate a `T(args...)` constructor call from the spelled-out
    // parameter type instead, which reported "call to unknown function
    // 'S'" for every aggregate and is the same fabricated-call pattern
    // removed elsewhere.
    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_default_argument_expr(const Type& param_type) {
        if (!check(TokenKind::LBrace)) return parse_expr();
        SourceLocation loc = current_loc();
        auto args_result = parse_brace_initializer_args();
        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
        std::vector<ExprPtr> args = std::move(args_result).value();
        if (args.empty()) return make_value_initialized_expr(loc, param_type);
        return make_braced_init_list_expr(loc, std::move(args));
    }

    // Builds the implicit `this` parameter every class member function
    // gets as params[0] (ch05 §5.9): a Reference to `class_name` --
    // `const T&` for a `const` method (or always for a destructor, which
    // needs to mutate/tear down the receiver, so never `const`... wait,
    // no: a destructor is never `const` either way, callers pass
    // `is_const=false` for it directly), `T&` otherwise. This -- an
    // ordinary Reference-typed parameter -- is the *entire* mechanism
    // scpp needs for `this`: every existing reference/borrow-checking
    // rule (elision, dangling checks, alias-XOR-mutability) already
    // applies with no new logic once a method is shaped this way (see
    // ClassDef's own comment).
    Param make_this_param(const std::string& class_name, bool is_const) {
        Param this_param{};

        this_param.name = "this";
        Type this_type{};

        this_type.kind = TypeKind::Reference;
        this_type.pointee = std::make_shared<Type>();
        this_type.pointee->kind = TypeKind::Named;
        this_type.pointee->name = class_name;
        this_type.pointee->is_const_qualified = is_const;
        this_type.is_mutable_ref = !is_const;
        this_param.type = std::move(this_type);
        return this_param;
    }

    // Prepends a synthesized `this` parameter to an already-parsed
    // parameter list (e.g. a constructor/operator's explicit,
    // user-written parameters), so it becomes params[0] as ch05 §5.9
    // requires. scpp's self-hosting compiler doesn't yet support
    // std::vector::insert (nor moving an individual vector element via
    // std::move(vec.at(i))/std::make_move_iterator), so this rebuilds the
    // list from scratch instead: a fresh vector seeded with `this`, then
    // every existing parameter copied in after it (Param is
    // copy-constructible, and a parameter list is a tiny, one-time
    // structure built during parsing, so the extra copy is a non-issue).
    [[nodiscard]] std::vector<Param> prepend_this_param(const std::vector<Param>& params, const std::string& class_name,
                                                        bool is_const) {
        std::vector<Param> result{};
        result.push_back(make_this_param(class_name, is_const));
        for (std::size_t i = 0; i < params.size(); i++) {
            result.push_back(params.at(i));
        }
        return result;
    }

    ReceiverRefQualifier parse_optional_ref_qualifier() {
        if (match(TokenKind::AmpAmp)) return ReceiverRefQualifier::RValue;
        if (match(TokenKind::Amp)) return ReceiverRefQualifier::LValue;
        return ReceiverRefQualifier::None;
    }

    [[nodiscard]] std::expected<void, ParseError> parse_function_trailing_attributes(Function& fn, const char* what) {
        const Token& attr_start_tok = peek();
        auto attrs_result = parse_attribute_specifier_seq();
        if (!attrs_result.has_value()) return std::unexpected(std::move(attrs_result).error());
        ParsedAttributes attrs = std::move(attrs_result).value();
        if (auto _rv = reject_packed_attribute(attrs, attr_start_tok, what); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (auto _rv = merge_lifetime_attribute(fn.return_lifetime, attrs.lifetime, attr_start_tok, what); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        LifetimeAnnotation return_lifetime_for_hoist = fn.return_lifetime;
        if (auto _rv = hoist_type_lifetime_annotation(fn.return_type, return_lifetime_for_hoist, attr_start_tok, what); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        fn.return_lifetime = std::move(return_lifetime_for_hoist);
        return {};
    }

    // [dcl.fct.def.default], [dcl.fct.def.delete] and [class.abstract]/2:
    // the three things a declarator may end with instead of a body. One
    // implementation, because "may this be written `= delete`?" is one
    // question that had four independent answers -- three of them
    // character-for-character copies of each other that all knew about
    // `default` and `0` and none of which knew about `delete`, and a
    // fourth (the out-of-line one) that knew only about `default`.
    //
    // `allow_pure` is false exactly where a pure-specifier is not
    // grammatically available (an out-of-line definition, a free
    // function); `= delete` is available everywhere a function is
    // declared, which is why it takes no flag.
    [[nodiscard]] std::expected<void, ParseError> parse_deleted_defaulted_or_pure_suffix(Function& fn, bool allow_default,
                                                                                         bool allow_pure,
                                                                                         const char* entity) {
        if (allow_default && match(TokenKind::KwDefault)) {
            fn.is_defaulted = true;
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            fn.body = nullptr;
            return {};
        }
        if (match(TokenKind::KwDelete)) {
            fn.is_deleted = true;
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            fn.body = nullptr;
            return {};
        }
        if (allow_pure && check(TokenKind::IntegerLiteral) && peek().text == "0") {
            advance();
            fn.is_pure = true;
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            fn.body = nullptr;
            return {};
        }
        const Token& tok = peek();
        {
            std::string _msg_delete_suffix{"expected "};
            if (allow_pure) {
                _msg_delete_suffix += "'default', 'delete' or '0'";
            } else if (allow_default) {
                _msg_delete_suffix += "'default' or 'delete'";
            } else {
                _msg_delete_suffix += "'delete'";
            }
            _msg_delete_suffix += " after '=' in ";
            _msg_delete_suffix += entity;
            return std::unexpected(ParseError(tok.line, tok.column, _msg_delete_suffix));
        }
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_member_function_suffix(Function& fn) {
        if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        fn.is_override = match(TokenKind::KwOverride);
        if (match(TokenKind::Assign)) {
            if (auto _rv = parse_deleted_defaulted_or_pure_suffix(fn, /*allow_default=*/true, /*allow_pure=*/true, "a member declaration");
                !_rv.has_value()) {
                return std::unexpected(std::move(_rv).error());
            }
            return std::unique_ptr<Stmt>{};
        }
        if (match(TokenKind::Semicolon)) return std::unique_ptr<Stmt>{};
        return parse_block();
    }

    // One element of a brace-enclosed initializer list. An element that
    // is itself a brace-enclosed list -- the inner `{1, 2}` of
    // `int a[2][2]{{1, 2}, {3, 4}}` -- is parsed by recursing here
    // rather than through parse_expr, which has no production for `{`
    // and rejected every such list with "expected an expression but
    // found '{'". The nested list becomes a BracedInitList expression,
    // whose meaning is supplied by whatever initialization boundary
    // consumes it, since a braced list has no type of its own.
    // Also the production for every position where a brace-enclosed
    // initializer list may stand in for an expression whose type comes
    // from context rather than from the list: `= {...}`, `return {...}`
    // and a call argument. [dcl.init.list] calls these
    // copy-list-initialization; what they share is that the target type
    // is known to the *consumer*, which is exactly what a BracedInitList
    // needs and exactly what it does not carry itself.
    [[nodiscard]] ExprPtr make_braced_init_list_expr(SourceLocation loc, std::vector<ExprPtr> args) {
        auto list = std::make_unique<Expr>();
        list->kind = ExprKind::BracedInitList;
        list->loc = loc;
        list->args = std::move(args);
        return list;
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_brace_initializer_element() {
        if (check(TokenKind::LBrace)) {
            SourceLocation nested_loc = current_loc();
            auto nested_result = parse_brace_initializer_args();
            if (!nested_result.has_value()) return std::unexpected(std::move(nested_result).error());
            std::vector<ExprPtr> nested_args = std::move(nested_result).value();
            return make_braced_init_list_expr(nested_loc, std::move(nested_args));
        }
        return parse_expr();
    }

    [[nodiscard]] std::expected<std::vector<ExprPtr>, ParseError> parse_brace_initializer_args() {
        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::vector<ExprPtr> args{};

        if (!check(TokenKind::RBrace)) {
            while (true) {
                auto arg_result = parse_brace_initializer_element();
                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                ExprPtr __arg_result_value = std::move(arg_result).value();
                args.push_back(std::move(__arg_result_value));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return std::move(args);
    }

    [[nodiscard]] std::expected<std::vector<ExplicitTemplateArg>, ParseError> parse_explicit_template_args(const std::vector<GenericTypeParam>& template_params) {
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::vector<ExplicitTemplateArg> explicit_template_args{};

        std::size_t arg_index = 0;
        if (!check(TokenKind::Greater)) {
            while (true) {
                ExplicitTemplateArg arg{};

                bool is_non_type =
                    arg_index < template_params.size() && template_params[arg_index].is_non_type;
                if (is_non_type) {
                    arg.is_type = false;
                    auto additive_result = parse_additive();
                    if (!additive_result.has_value()) return std::unexpected(std::move(additive_result).error());
                    arg.value = std::shared_ptr<Expr>(std::move(additive_result).value().release());
                } else if (arg_index < template_params.size() && template_params[arg_index].is_pack &&
                           check(TokenKind::Identifier) && peek_at(1).kind == TokenKind::Ellipsis) {
                    arg.is_type = true;
                    arg.type.kind = TypeKind::Named;
                    const Token& pack_name_tok = advance();
                    arg.type.name = std::string(pack_name_tok.text.data(), pack_name_tok.text.size());
                    advance(); // '...'
                    arg.type.is_pack_expansion = true;
                } else {
                    arg.is_type = true;
                    auto _tmp_result = parse_template_type_argument();
                    if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
                    arg.type = std::move(_tmp_result).value();
                }
                explicit_template_args.push_back(std::move(arg));
                arg_index++;
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return explicit_template_args;
    }

    [[nodiscard]] std::expected<std::optional<std::vector<ExplicitTemplateArg>>, ParseError>
    try_parse_explicit_generic_type_constructor_template_args(const std::string& spelled_name,
                                                              bool explicit_global_qualification) {
        std::string resolved_name{};
        if (explicit_global_qualification) {
            resolved_name = spelled_name;
        } else {
            resolved_name = resolve_visible_type_name(spelled_name);
        }
        if (resolved_name.empty() || !generic_type_names_.contains(resolved_name) || !check(TokenKind::Less)) {
            return std::optional<std::vector<ExplicitTemplateArg>>{};
        }
        bool has_ordinary_params = ordinary_generic_type_template_params_.contains(resolved_name);
        bool has_variadic_params = !has_ordinary_params && variadic_primary_template_params_.contains(resolved_name);
        if (!has_ordinary_params && !has_variadic_params) return std::optional<std::vector<ExplicitTemplateArg>>{};

        if (has_ordinary_params) {
            return continue_parsing_explicit_generic_type_constructor_template_args(
                ordinary_generic_type_template_params_.at(resolved_name));
        }
        return continue_parsing_explicit_generic_type_constructor_template_args(
            variadic_primary_template_params_.at(resolved_name));
    }

    [[nodiscard]] std::expected<std::optional<std::vector<ExplicitTemplateArg>>, ParseError>
    continue_parsing_explicit_generic_type_constructor_template_args(const std::vector<GenericTypeParam>& template_params) {
        std::size_t saved_pos = pos_;
        auto args_result = parse_explicit_template_args(template_params);
        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
        std::vector<ExplicitTemplateArg> args = std::move(args_result).value();
        if (!check(TokenKind::LParen) && !check(TokenKind::LBrace)) {
            pos_ = saved_pos;
            return std::optional<std::vector<ExplicitTemplateArg>>{};
        }
        std::optional<std::vector<ExplicitTemplateArg>> args_opt{std::move(args)};
        return args_opt;
    }

    [[nodiscard]] std::expected<std::optional<Initializer>, ParseError> parse_optional_default_initializer(const std::string& thing_name) {
        if (match(TokenKind::Assign)) {
            Initializer init{};

            auto default_init_expr_result = parse_expr();
            if (!default_init_expr_result.has_value()) return std::unexpected(std::move(default_init_expr_result).error());
            init.expr = std::move(default_init_expr_result).value();
            std::optional<Initializer> init_opt{std::move(init)};
            return init_opt;
        }
        if (check(TokenKind::LBrace)) {
            Initializer init{};

            init.has_brace_args = true;
            auto default_init_brace_args_result = parse_brace_initializer_args();
            if (!default_init_brace_args_result.has_value()) return std::unexpected(std::move(default_init_brace_args_result).error());
            init.brace_args = std::move(default_init_brace_args_result).value();
            std::optional<Initializer> init_opt{std::move(init)};
            return init_opt;
        }
        if (match(TokenKind::LParen)) {
            const Token& tok = peek();
            {
                std::string _msg_5326{"parenthesized direct-initialization is not allowed for "};
                _msg_5326 += thing_name;
                _msg_5326 += "; use brace-init instead";
                return std::unexpected(ParseError(tok.line, tok.column,
                             _msg_5326));
            }
        }
        return std::optional<Initializer>{};
    }

    [[nodiscard]] std::expected<std::vector<MemberInitializer>, ParseError> parse_constructor_member_initializer_list() {
        std::vector<MemberInitializer> initializers{};

        if (!match(TokenKind::Colon)) return initializers;
        std::unordered_set<std::string> seen_members{};

        while (true) {
            auto name_tok_result = expect(TokenKind::Identifier, "member name");
            if (!name_tok_result.has_value()) return std::unexpected(std::move(name_tok_result).error());
            const Token& name_tok = std::move(name_tok_result).value();
            MemberInitializer init{};

            init.member_name = std::string(name_tok.text.data(), name_tok.text.size());
            init.loc = make_source_location(name_tok.line, name_tok.column, source_path_);
            // std::unordered_set::insert's return type differs between
            // scpp's self-hosting implementation (a plain bool) and real
            // std::unordered_set (std::pair<iterator, bool>) -- rather
            // than relying on insert's return value at all (which would
            // need a different access idiom, ".second" vs. none, per
            // compiler), check membership with contains() first (bool in
            // both), then insert separately.
            if (seen_members.contains(init.member_name)) {
                {
                    std::string _msg_5348{"member '"};
                    _msg_5348 += init.member_name;
                    _msg_5348 += "' cannot appear more than once in the same constructor member-initializer-list";
                    return std::unexpected(ParseError(name_tok.line, name_tok.column,
                                 _msg_5348));
                }
            }
            seen_members.insert(init.member_name);
            if (check(TokenKind::LParen)) {
                const Token& tok = peek();
                {
                    std::string _msg_5354{"parenthesized expression-lists are not allowed in a constructor member-initializer; "};
                    _msg_5354 += "use brace-init instead";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                 _msg_5354));
                }
            }
            init.initializer.has_brace_args = true;
            auto member_init_brace_args_result = parse_brace_initializer_args();
            if (!member_init_brace_args_result.has_value()) return std::unexpected(std::move(member_init_brace_args_result).error());
            init.initializer.brace_args = std::move(member_init_brace_args_result).value();
            initializers.push_back(std::move(init));
            if (!(match(TokenKind::Comma))) break;
        }
        return initializers;
    }

    // ch05 §5.11: generic (concept-constrained) *methods* aren't
    // supported in v0.1 -- only free functions are (matching every
    // example the spec itself demonstrates). Rather than silently
    // mismarking such a method (parse_param_list's "ConceptName auto"
    // syntax is shared with parse_function, so it would otherwise parse
    // without error, just with is_generic_template never set -- an
    // ordinary-looking Function that's actually unsound to compile as
    // one), reject it outright with a clear, scoped error message.
    [[nodiscard]] std::expected<void, ParseError> reject_generic_params(const std::vector<Param>& params, const std::string& what) {
        for (const Param& param : params) {
            if (!param.generic_concept.empty()) {
                {
                    std::string _msg_5379{"a generic (concept-constrained) parameter is not supported on "};
                    _msg_5379 += what;
                    _msg_5379 += " in this version (ch05 §5.11 -- only a free function may be generic)";
                    return std::unexpected(ParseError(current_loc().line, current_loc().column,
                                  _msg_5379));
                }
            }
        }
        return {};
    }

    // ch05 §5.11: parses a single requirement inside a concept's
    // requires-expression body -- restricted (a pragmatic v0.1 scoping
    // cut matching the spec's own examples) to a call on the concept's
    // own placeholder parameter, never an arbitrary expression. Mirrors
    // real C++20 requires-expression grammar exactly: a *simple*
    // requirement is a bare expression-statement with **no** braces at
    // all (`f(x);`) and can never carry a `->` constraint; only a
    // *compound* requirement is brace-wrapped (`{ t.area() } ->
    // std::same_as<T>;`), and a compound requirement's braces always
    // require exactly the `-> constraint` that follows them (v0.1 has no
    // use for a brace-wrapped requirement with no `->`, e.g. a bare
    // `noexcept` check -- not part of this feature's scope):
    //   <placeholder>.<method>(<args>);                          --
    //     simple, a method call.
    //   { <placeholder>.<method>(<args>) } -> std::same_as<T>;   --
    //     compound, constraining the call's result to exact type T.
    //   <placeholder>(<args>);                                   --
    //     simple, *directly invoking* the placeholder itself (e.g.
    //     IntConsumer's `f(x)`) -- modeled internally as a call to a
    //     fixed synthesized method name ("call"), exactly like a
    //     closure's own compiler-synthesized operator() (ch05 §5.12):
    //     both are resolved through the same "a bare Call redirects to
    //     receiver.call(args) when the callee name is a class-typed
    //     value with a 'call' method" sugar, so a concept requiring
    //     direct invocation and a real closure satisfying it line up
    //     with zero extra machinery.
    // Every argument must be a bare reference to one of the requires-
    // expression's own *other* (non-placeholder) parameters -- resolved
    // to a concrete Type via `helper_param_types` right here. Each such
    // parameter's own `[[scpp::lifetime(...)]]` annotation (if any),
    // looked up the same way via `helper_param_lifetimes`, *does* survive
    // into ConceptRequirement::arg_lifetimes -- spec §6.2(22) makes it
    // constrain concept satisfaction itself (see generics_support.cppm's
    // type_satisfies_concept).
    //
    // spec §13.2 additionally recognizes a *construction*-shaped
    // requirement -- `T(args...);` / `T{args...};` (simple), or
    // `{ T(args...) };` / `{ T{args...} };` (compound, never with a
    // trailing `-> constraint` -- spec §13.2(3)'s own precondition),
    // where `T` names a type (almost always the concept's own
    // `template_param_name`) rather than the requires-expression's
    // placeholder -- alongside the call-shaped forms above; see
    // parse_construction_shaped_concept_requirement below. This is only
    // ever recognized when the leading identifier is *not* the
    // placeholder's own name -- a placeholder-led expression always
    // means one of the call-shaped forms above, exactly as before.
    [[nodiscard]] std::expected<ConceptRequirement, ParseError> parse_concept_requirement(
        const std::string& placeholder_name, const std::string& template_param_name,
        const std::unordered_map<std::string, Type>& helper_param_types,
        const std::unordered_map<std::string, LifetimeAnnotation>& helper_param_lifetimes) {
        ConceptRequirement req{};

        bool is_compound = match(TokenKind::LBrace);

        const Token& receiver_tok = peek();
        bool leading_is_placeholder =
            receiver_tok.kind == TokenKind::Identifier && receiver_tok.text == placeholder_name;
        if (!leading_is_placeholder && receiver_tok.kind == TokenKind::Identifier &&
            (receiver_tok.text == template_param_name || is_visible_type_name(std::string(receiver_tok.text.data(), receiver_tok.text.size()))) &&
            (peek_at(1).kind == TokenKind::LParen || peek_at(1).kind == TokenKind::LBrace)) {
            return parse_construction_shaped_concept_requirement(is_compound, placeholder_name, template_param_name,
                                                                  helper_param_types, helper_param_lifetimes);
        }

        if (auto _r = expect(TokenKind::Identifier, "the concept's own requires-parameter name"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (receiver_tok.text != placeholder_name) {
            {
                std::string _msg_5453{"expected a requirement shaped as a call on '"};
                _msg_5453 += placeholder_name;
                _msg_5453 += "' (this concept's own constrained requires-parameter) -- v0.1 does not ";
                _msg_5453 += "support an arbitrary requirement expression";
                return std::unexpected(ParseError(receiver_tok.line, receiver_tok.column,
                              _msg_5453));
            }
        }
        if (match(TokenKind::Dot)) {
            auto method_name_result = expect(TokenKind::Identifier, "method name");
            if (!method_name_result.has_value()) return std::unexpected(std::move(method_name_result).error());
            req.method_name = std::string(method_name_result.value().text.data(), method_name_result.value().text.size());
        } else {
            req.method_name = "call";
        }
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!check(TokenKind::RParen)) {
            while (true) {
                auto arg_tok_result = expect(TokenKind::Identifier, "a requirement argument (a requires-expression parameter name)");
                if (!arg_tok_result.has_value()) return std::unexpected(std::move(arg_tok_result).error());
                const Token& arg_tok = arg_tok_result.value();
                if (!helper_param_types.contains(std::string(arg_tok.text.data(), arg_tok.text.size()))) {
                    {
                        std::string _msg_5473{"'"};
                        _msg_5473 += std::string(arg_tok.text.data(), arg_tok.text.size());
                        _msg_5473 += "' is not one of this concept's own requires-expression parameters -- ";
                        _msg_5473 += "v0.1 only supports a requirement argument that is a bare reference to ";
                        _msg_5473 += "one of them";
                        return std::unexpected(ParseError(arg_tok.line, arg_tok.column,
                                      _msg_5473));
                    }
                }
                req.arg_types.push_back(helper_param_types.at(std::string(arg_tok.text.data(), arg_tok.text.size())));
                req.arg_lifetimes.push_back(helper_param_lifetimes.at(std::string(arg_tok.text.data(), arg_tok.text.size())));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        if (is_compound) {
            if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            // ch05 §5.11: the result must be constrained to an *exact*
            // type -- 'std::same_as<T>' only, never
            // 'std::convertible_to<T>' (scpp has no implicit scalar
            // conversions at all, so the two would mean the same thing
            // anyway).
            {
                std::string _msg_5493{std::string("'->' (a brace-wrapped requirement must be followed by a 'std::same_as<T>' constraint in ")};
                _msg_5493 += "this version)";
                if (auto _r = expect(TokenKind::Arrow,
                   _msg_5493); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            if (!check_std_qualified("same_as")) {
                const Token& tok = peek();
                {
                    std::string _msg_5498{"a compound requirement's constraint must be 'std::same_as<T>' in this "};
                    _msg_5498 += "version (never 'std::convertible_to<T>')";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_5498));
                }
            }
            consume_std_qualified();
            if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto _tmp_result = parse_type();
            if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
            req.return_type = std::move(_tmp_result).value();
            if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            req.has_return_constraint = true;
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return req;
    }

    // spec §13.2(3.3)/(4): parses a construction-shaped requirement,
    // `T(args...);` / `T{args...};` (simple) or `{ T(args...) };` /
    // `{ T{args...} };` (compound) -- called by parse_concept_requirement
    // only once its own lookahead has already confirmed the leading
    // token is a type name (not the requires-expression's own
    // placeholder) immediately followed by '(' or '{', so every
    // `expect`/`check` call below is only ever reached in a context
    // where it's already known to succeed on the *shape*, not just
    // hoped for. `is_compound` mirrors parse_concept_requirement's own
    // (already-consumed) leading-brace check -- when true, an extra
    // outer '}' must be consumed before the trailing ';'. Per spec
    // §13.2(3)'s own precondition ("no type-constraint follows the
    // closing }"), this shape can never carry a trailing `->
    // std::same_as<T>` constraint the way the call-shaped compound forms
    // in parse_concept_requirement can -- has_return_constraint is
    // always left false.
    [[nodiscard]] std::expected<ConceptRequirement, ParseError> parse_construction_shaped_concept_requirement(
        bool is_compound, const std::string& placeholder_name, const std::string& template_param_name,
        const std::unordered_map<std::string, Type>& helper_param_types,
        const std::unordered_map<std::string, LifetimeAnnotation>& helper_param_lifetimes) {
        ConceptRequirement req{};

        req.is_construct = true;
        auto construct_type_name_result = expect(TokenKind::Identifier, "a constructed type name");
        if (!construct_type_name_result.has_value()) return std::unexpected(std::move(construct_type_name_result).error());
        req.construct_type_name = std::string(construct_type_name_result.value().text.data(), construct_type_name_result.value().text.size());

        bool paren_form = match(TokenKind::LParen);
        if (!paren_form) { if (auto _r = expect(TokenKind::LBrace, "'(' or '{'"); !_r.has_value()) return std::unexpected(std::move(_r).error()); }
        TokenKind close_kind = paren_form ? TokenKind::RParen : TokenKind::RBrace;
        if (!check(close_kind)) {
            while (true) {
                auto arg_tok_result = expect(TokenKind::Identifier, "a requirement argument (a requires-expression parameter name)");
                if (!arg_tok_result.has_value()) return std::unexpected(std::move(arg_tok_result).error());
                const Token& arg_tok = arg_tok_result.value();
                if (arg_tok.text == placeholder_name) {
                    // spec's own note: this is the whole point of the
                    // construction shape -- probing the placeholder's
                    // own type's constructibility (e.g. `T{t}` for
                    // copy-constructibility). Represented as a Named
                    // type spelled exactly like the concept's own
                    // template_param_name, substituted for the concrete
                    // type under test at concept-satisfaction time (see
                    // generics_support.cppm's type_satisfies_concept).
                    Type placeholder_type{};

                    placeholder_type.kind = TypeKind::Named;
                    placeholder_type.name = template_param_name;
                    req.arg_types.push_back(std::move(placeholder_type));
                    req.arg_lifetimes.push_back(LifetimeAnnotation{});
                } else {
                    if (!helper_param_types.contains(std::string(arg_tok.text.data(), arg_tok.text.size()))) {
                        {
                            std::string _msg_5567{"'"};
                            _msg_5567 += std::string(arg_tok.text.data(), arg_tok.text.size());
                            _msg_5567 += "' is not one of this concept's own requires-expression parameters ";
                            _msg_5567 += "-- v0.1 only supports a requirement argument that is a bare ";
                            _msg_5567 += "reference to one of them (or the placeholder itself)";
                            return std::unexpected(ParseError(arg_tok.line, arg_tok.column,
                                          _msg_5567));
                        }
                    }
                    req.arg_types.push_back(helper_param_types.at(std::string(arg_tok.text.data(), arg_tok.text.size())));
                    req.arg_lifetimes.push_back(helper_param_lifetimes.at(std::string(arg_tok.text.data(), arg_tok.text.size())));
                }
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(close_kind, paren_form ? "')'" : "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (is_compound) {
            if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            if (check(TokenKind::Arrow)) {
                const Token& tok = peek();
                {
                    std::string _msg_5584{"a construction-shaped compound requirement ('"};
                    _msg_5584 += req.construct_type_name;
                    _msg_5584 += "(...)' or '";
                    _msg_5584 += req.construct_type_name;
                    _msg_5584 += "{...}') cannot be followed by a '-> constraint' in this version (spec ";
                    _msg_5584 += "§13.2(3) only recognizes this shape for a requirement with no trailing ";
                    _msg_5584 += "type-constraint)";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_5584));
                }
            }
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return req;
    }

    // ch05 §5.11: parses `template<typename T> concept Name =
    // requires(...) { ... };` -- the *only* place v0.1 ever uses a full
    // `template<...>` header (a generic *function* stays abbreviated-
    // form-only, `Concept auto`); real C++ grammar has no other way to
    // declare a concept at all, so this is unavoidable even though the
    // rest of this feature deliberately avoids the general template-
    // parameter machinery.
    //
    // Immediately synthesizes the concept's hidden witness class (one
    // bodyless method per requirement, named via the same
    // `ClassName_memberName` scheme every other method uses) directly
    // into `program` -- see ClassDef::is_concept_witness and
    // Function::is_generic_template's own comments for why this lets a
    // constrained generic function's body-check reuse 100% of the
    // existing class/method-call machinery with zero new logic.
    //
    // ch05 §5.14: parses a generic `class`/`struct` type's own
    // `template<...>` header: zero or more comma-separated parameters,
    // each either a *type* parameter (`typename Name`, bare; `Concept
    // Name`, constrained -- real C++20 syntax, a concept may appear
    // directly in a template parameter list as shorthand for `typename
    // Name` plus a matching `requires` clause), a *pack* of one
    // (`typename... Name`, legal only as the last parameter -- a
    // variadic primary template's own header, e.g. `template<typename...
    // Ts> class Tuple;`), or a *non-type* parameter (a scalar type
    // followed by a name, e.g. `int Idx` -- restricted to whatever
    // scalar types this version already supports; `size_t`/`ptrdiff_t`/
    // fixed-width integers don't exist as scpp types yet, so `int` is
    // used in their place for now). The caller (parse_top_level_item)
    // has already confirmed, via lookahead past the whole header, that
    // this is a `class`/`struct` header rather than a `concept` one,
    // but hasn't consumed anything yet.
    [[nodiscard]] std::expected<std::vector<GenericTypeParam>, ParseError> parse_generic_type_header() {
        if (auto _r = expect(TokenKind::KwTemplate, "'template'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::vector<GenericTypeParam> params{};

        if (!check(TokenKind::Greater)) {
            while (true) {
                GenericTypeParam param{};

                if (match(TokenKind::KwTypename)) {
                    param.is_pack = match(TokenKind::Ellipsis);
                    auto name_result = expect(TokenKind::Identifier, "template parameter name");
                    if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
                    param.name = std::string(name_result.value().text.data(), name_result.value().text.size());
                } else if (check(TokenKind::KwInt) || check(TokenKind::KwBool) || check(TokenKind::KwChar)) {
                    // ch05 §5.14: a non-type parameter -- restricted to
                    // scalar types (only int/bool/char exist as scpp
                    // types so far; ptrdiff_t/fixed-width integers/
                    // float32_t/float64_t/size_t are all deferred until
                    // those types themselves exist).
                    param.is_non_type = true;
                    auto _tmp_result = parse_unqualified_type();
                    if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
                    param.non_type_type = std::move(_tmp_result).value();
                    auto name_result = expect(TokenKind::Identifier, "template parameter name");
                    if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
                    param.name = std::string(name_result.value().text.data(), name_result.value().text.size());
                } else {
                    const Token& concept_tok = peek();
                    auto concept_name_tok_result = expect(TokenKind::Identifier, "'typename', a scalar type, or a concept name");
                    if (!concept_name_tok_result.has_value()) return std::unexpected(std::move(concept_name_tok_result).error());
                    std::string concept_name{concept_name_tok_result.value().text.data(), concept_name_tok_result.value().text.size()};
                    if (!concept_names_.contains(concept_name)) {
                        {
                            std::string _msg_5661{"'"};
                            _msg_5661 += concept_name;
                            _msg_5661 += "' is not a declared concept -- a generic type's template ";
                            _msg_5661 += "parameter must be introduced by 'typename', a scalar type, or ";
                            _msg_5661 += "an already-declared concept name (ch05 §5.14)";
                            return std::unexpected(ParseError(concept_tok.line, concept_tok.column,
                                          _msg_5661));
                        }
                    }
                    param.concept_name = concept_name;
                    auto name_result = expect(TokenKind::Identifier, "template parameter name");
                    if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
                    param.name = std::string(name_result.value().text.data(), name_result.value().text.size());
                }
                if (param.is_pack && !check(TokenKind::Greater)) {
                    const Token& tok = peek();
                    {
                        std::string _msg_5674{"a parameter pack ('typename... "};
                        _msg_5674 += param.name;
                        _msg_5674 += "') must be the last template parameter (ch05 §5.14)";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_5674));
                    }
                }
                params.push_back(std::move(param));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return params;
    }

    // ch05 §5.14: parses a generic method (or constructor)'s own,
    // optional `requires ConceptName<T>` clause -- real C++20 syntax
    // verbatim, appearing after the parameter list (and, for a method,
    // its trailing `const`) and before the body. `T` must name the
    // enclosing generic type's own single template parameter exactly
    // (this version has only one to match). `ConceptName` may be
    // namespace-qualified (e.g. `std::copy_constructible`) -- resolved
    // via resolve_visible_concept_name exactly like an ordinary type
    // name would be, so a concept declared inside `namespace std { ...
    // }` (e.g. std_concepts.scpp's own std::copy_constructible) can be
    // named either that way or, from within namespace std itself, bare.
    // Returns the concept's own fully-qualified name
    // (Function::method_requires_concept), or empty if no such clause is
    // present -- always empty when `template_params` itself is empty (an
    // ordinary, non-generic class/struct's member can never have one,
    // since there's no type parameter left to constrain).
    [[nodiscard]] std::expected<std::string, ParseError> parse_optional_method_requires_clause(const std::vector<GenericTypeParam>& template_params) {
        if (template_params.empty() || !check(TokenKind::KwRequires)) { std::string empty_result{}; return empty_result; }
        advance(); // 'requires'
        const Token& concept_tok = peek();
        std::string spelled_concept_name = peek_qualified_name();
        if (spelled_concept_name.empty()) { if (auto _r = expect(TokenKind::Identifier, "concept name"); !_r.has_value()) return std::unexpected(std::move(_r).error()); }
        std::string concept_name = resolve_visible_concept_name(spelled_concept_name);
        if (concept_name.empty()) {
            {
                std::string _msg_5710{"'"};
                _msg_5710 += spelled_concept_name;
                _msg_5710 += "' is not a declared concept (ch05 §5.14)";
                return std::unexpected(ParseError(concept_tok.line, concept_tok.column,
                              _msg_5710));
            }
        }
        parse_qualified_name(); // now actually consume it, having already resolved it above
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        const Token& param_tok = peek();
        auto arg_name_result = expect(TokenKind::Identifier, "the generic type's own template parameter name");
        if (!arg_name_result.has_value()) return std::unexpected(std::move(arg_name_result).error());
        std::string arg_name{arg_name_result.value().text.data(), arg_name_result.value().text.size()};
        if (arg_name != template_params[0].name) {
            {
                std::string _msg_5720{"'requires "};
                _msg_5720 += concept_name;
                _msg_5720 += "<";
                _msg_5720 += arg_name;
                _msg_5720 += ">' does not name this generic type's own template parameter '";
                _msg_5720 += template_params[0].name;
                _msg_5720 += "' (ch05 §5.14)";
                return std::unexpected(ParseError(param_tok.line, param_tok.column,
                              _msg_5720));
            }
        }
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return concept_name;
    }

    // ch05 §5.11: `template<...> ReturnType name(params) { body }` -- a
    // generic function spelled with the full header form (as opposed to
    // the abbreviated `Concept auto` form, parse_top_level_function_or_
    // extern_group's own ordinary path). Reuses parse_function verbatim
    // for the return type/name/params/body (identical grammar to an
    // ordinary function from that point on) -- the only difference is
    // temporarily registering each *type*-kind template parameter's own
    // name (bare or concept-constrained, and a pack's own name too) as a
    // type name for the duration of parsing this one function's own
    // signature and body, exactly mirroring parse_class_def/
    // parse_variadic_specialization's identical established pattern (a
    // *non-type* parameter's own name, e.g. "I", needs no such
    // registration: it's referenced as a bare value expression, not a
    // type, and the parser never validates a non-type-argument
    // expression's own identifier references at parse time at all --
    // see this function's own non_type_args comment on Type). A pack
    // parameter's own name (e.g. "Tail") is registered exactly like an
    // ordinary type parameter's, even though it's only ever legally
    // *used* spread (`Tail...`) inside a base-class-deduction pattern
    // parameter type -- nothing at this point needs to specially
    // reject a bare, non-spread reference to it (there is no legal
    // function-parameter position for one anyway, so a bare `Tail x`
    // parameter would simply never resolve to anything real at
    // monomorphization time, surfacing there instead).
    [[nodiscard]] std::expected<void, ParseError> parse_generic_function_def(Program& program, bool is_exported, bool is_unsafe = false,
                                    bool is_nodiscard = false, const std::string& nodiscard_reason = {}) {
        SourceLocation loc = current_loc();
        auto template_params_result = parse_generic_type_header();
        if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
        std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.insert(p.name);
            class_names_.insert(p.name);
        }

        std::vector<GenericTypeParam> saved_template_params = current_function_template_params_;
        current_function_template_params_ = template_params;
        auto fn_result =
            parse_function(/*is_extern_c=*/false, /*is_module_extern=*/false, is_unsafe, is_nodiscard,
                           nodiscard_reason);
        current_function_template_params_ = std::move(saved_template_params);
        if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
        Function fn = std::move(fn_result).value();
        fn.loc = loc;
        fn.name = qualify_name(fn.name);
        fn.namespace_path = namespace_stack_;
        fn.is_exported = is_exported;
        fn.template_params = template_params;
        fn.is_generic_template = true;
        {
            std::string _msg_5778{"function '"};
            _msg_5778 += fn.name;
            _msg_5778 += "'";
            if (auto _rv = check_export_context(program, is_exported, fn.namespace_path, loc, _msg_5778); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        generic_function_template_params_.insert_or_assign(fn.name, template_params);

        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.erase(p.name);
            class_names_.erase(p.name);
        }
        program.functions.push_back(std::move(fn));
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_concept_def(Program& program, bool is_exported) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwTemplate, "'template'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::KwTypename, "'typename'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto template_param_name_result = expect(TokenKind::Identifier, "template parameter name");
        if (!template_param_name_result.has_value()) return std::unexpected(std::move(template_param_name_result).error());
        std::string template_param_name = std::string(template_param_name_result.value().text.data(), template_param_name_result.value().text.size());
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        if (auto _r = expect(TokenKind::KwConcept, "'concept'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        ConceptDef def{};

        auto bare_name_result = expect(TokenKind::Identifier, "concept name");
        if (!bare_name_result.has_value()) return std::unexpected(std::move(bare_name_result).error());
        std::string bare_name = std::string(bare_name_result.value().text.data(), bare_name_result.value().text.size());
        def.name = qualify_name(bare_name);
        def.template_param_name = template_param_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        {
            std::string _msg_5810{"concept '"};
            _msg_5810 += def.name;
            _msg_5810 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_5810); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        concept_names_.insert(def.name);
        // The witness class shares the concept's own fully-qualified
        // name -- a concept and a class/struct can never collide in
        // real C++ (different entity kinds sharing one namespace,
        // exactly like a class and a function can't share a name
        // either), so this is always unambiguous. Registering it the
        // same way parse_class_def does makes every existing type-name
        // lookup (looks_like_type_start, generic-parameter parsing,
        // ...) just work with no special-casing.
        struct_names_.insert(def.name);
        class_names_.insert(def.name);

        if (auto _r = expect(TokenKind::Assign, "'='"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::KwRequires, "'requires'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        // The requires-expression's own (fake, unevaluated) parameter
        // list: exactly one parameter's declared type must be
        // (optionally const-qualified) the template parameter itself --
        // e.g. `const T& t` -- identifying it as the constrained
        // placeholder (def.requires_param_name). Every other parameter is
        // an ordinary,
        // already-declared concrete type (e.g. `int x`), tracked only
        // transiently to resolve a requirement's own argument types.
        std::unordered_map<std::string, Type> helper_param_types{};

        // spec §6.2(13.1)/(22): a probe (helper) parameter's own
        // `[[scpp::lifetime(...)]]` annotation, tracked alongside its
        // type -- constrains concept satisfaction (see
        // parse_concept_requirement/type_satisfies_concept), unlike the
        // placeholder itself, which (like an ordinary function's implicit
        // object parameter, spec §6.2(23)) may never bear this attribute.
        std::unordered_map<std::string, LifetimeAnnotation> helper_param_lifetimes{};

        bool found_placeholder = false;
        if (!check(TokenKind::RParen)) {
            while (true) {
                std::size_t const_offset = static_cast<std::size_t>(check(TokenKind::KwConst) ? 1 : 0);
                bool is_placeholder = peek_at(const_offset).kind == TokenKind::Identifier &&
                                       peek_at(const_offset).text == template_param_name;
                if (is_placeholder) {
                    if (found_placeholder) {
                        const Token& tok = peek();
                        {
                            std::string _msg_5855{"a concept's requires-expression may only have one parameter of "};
                            _msg_5855 += "the constrained type '";
                            _msg_5855 += template_param_name;
                            _msg_5855 += "'";
                            return std::unexpected(ParseError(tok.line, tok.column,
                                          _msg_5855));
                        }
                    }
                    def.requires_param_is_const = match(TokenKind::KwConst);
                    advance(); // the template parameter name itself (e.g. "T")
                    match(TokenKind::Amp); // optional trailing '&' -- ref-ness itself
                                            // doesn't affect this v0.1
                                            // concept model
                    auto requires_param_name_result = expect(TokenKind::Identifier, "requires-parameter name");
                    if (!requires_param_name_result.has_value()) return std::unexpected(std::move(requires_param_name_result).error());
                    def.requires_param_name = std::string(requires_param_name_result.value().text.data(), requires_param_name_result.value().text.size());
                    found_placeholder = true;
                } else {
                    auto helper_type_result = parse_type_with_lifetime_attributes_enabled();
                    if (!helper_type_result.has_value()) return std::unexpected(std::move(helper_type_result).error());
                    Type helper_type = std::move(helper_type_result).value();
                    auto helper_name_result = expect(TokenKind::Identifier, "requires-parameter name");
                    if (!helper_name_result.has_value()) return std::unexpected(std::move(helper_name_result).error());
                    std::string helper_name = std::string(helper_name_result.value().text.data(), helper_name_result.value().text.size());
                    // ch05 §5.13/spec §6.2(13.1): a probe parameter
                    // accepts the same trailing attribute-specifier-seq
                    // an ordinary function parameter does (see
                    // parse_function's identical handling) -- most
                    // importantly `[[scpp::lifetime(name)]]`.
                    const Token& helper_attr_start_tok = peek();
                    auto helper_attrs_result = parse_attribute_specifier_seq();
                    if (!helper_attrs_result.has_value()) return std::unexpected(std::move(helper_attrs_result).error());
                    ParsedAttributes helper_attrs = std::move(helper_attrs_result).value();
                    if (auto _rv = reject_packed_attribute(helper_attrs, helper_attr_start_tok,
                                             "a requires-expression parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    LifetimeAnnotation helper_lifetime{};

                    if (auto _rv = merge_lifetime_attribute(helper_lifetime, helper_attrs.lifetime, helper_attr_start_tok,
                                              "a requires-expression parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    if (auto _rv = hoist_type_lifetime_annotation(helper_type, helper_lifetime, helper_attr_start_tok,
                                                  "a requires-expression parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                    helper_param_types.insert_or_assign(helper_name, std::move(helper_type));
                    helper_param_lifetimes.insert_or_assign(helper_name, std::move(helper_lifetime));
                }
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!found_placeholder) {
            const Token& tok = peek();
            {
                std::string _msg_5902{"concept '"};
                _msg_5902 += def.name;
                _msg_5902 += "'s requires-expression must have exactly one parameter of the ";
                _msg_5902 += "constrained type '";
                _msg_5902 += template_param_name;
                _msg_5902 += "'";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_5902));
            }
        }

        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            auto requirement_result = parse_concept_requirement(def.requires_param_name, template_param_name, helper_param_types,
                                          helper_param_lifetimes);
            if (!requirement_result.has_value()) return std::unexpected(std::move(requirement_result).error());
            ConceptRequirement __requirement_result_value = std::move(requirement_result).value();
            def.requirements.push_back(std::move(__requirement_result_value));
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        ClassDef witness{};

        witness.name = def.name;
        witness.namespace_path = namespace_stack_;
        witness.is_exported = is_exported;
        witness.is_concept_witness = true;
        program.classes.push_back(std::move(witness));

        for (const ConceptRequirement& req : def.requirements) {
            // spec §13.2: a construction-shaped requirement has no
            // "method" at all for a generic body to call against (see
            // type_satisfies_concept's own, entirely separate,
            // constructor-matching path for how it's actually checked)
            // -- so, unlike every call-shaped requirement above, it
            // synthesizes no per-requirement witness Function here.
            if (req.is_construct) continue;
            Function fn{};

            fn.loc = loc;
            fn.name = def.name;
            fn.name += "_";
            fn.name += req.method_name;
            fn.namespace_path = namespace_stack_;
            fn.is_exported = is_exported;
            fn.member_owner_class = def.name;
            fn.return_type =
                req.has_return_constraint ? req.return_type : named_type("void");
            // The witness class exists only for one-time abstract body
            // checking of a constrained generic before monomorphization.
            // A `requires(T t) { t.get(); }` simple requirement does not
            // itself guarantee const-callability, but real C++ still
            // permits a generic body to compile and later lets the
            // concrete instantiation decide whether a `const` access path
            // is valid. Model that the same way here by giving the witness
            // the read-only-capable shape; concrete monomorphized clones
            // are still checked again against the real type and will
            // reject an actually non-const-only method when instantiated
            // through a const access path.
            fn.params.push_back(make_this_param(def.name, /*is_const=*/true));
            for (std::size_t i = 0; i < req.arg_types.size(); i++) {
                Param p{};

                p.type = req.arg_types[i];
                p.name = "arg";
                p.name += std::to_string(i);
                // Carries the probe parameter's own lifetime annotation
                // (if any) through to the witness's signature -- this
                // both (a) reuses build_signatures'
                // validate_lifetime_annotation_placement to reject an
                // ill-formed probe (tagged but not reference/pointer/
                // span-typed) exactly like an ordinary parameter would
                // be, and (b) is otherwise unused by the witness itself
                // (is_concept_witness excludes it from real dataflow
                // checking) -- the actual (22.1)-(22.4) satisfaction
                // matching reads req.arg_lifetimes directly (see
                // type_satisfies_concept), not this copy.
                p.lifetime = req.arg_lifetimes[i];
                fn.params.push_back(std::move(p));
            }
            // Bodyless: a witness method is never actually called or
            // compiled (is_concept_witness excludes it from codegen
            // entirely) -- it exists purely as a signature for the
            // generic function's own abstract body-check to resolve
            // calls against.
            fn.body = nullptr;
            program.functions.push_back(std::move(fn));
        }

        program.concepts.push_back(std::move(def));
        return {};
    }

    // Parses `class Name { ... };` (ch04 §4.2/ch05 §5.9): fields (with
    // access-specifier sections, defaulting to `private` like real C++,
    // unlike `struct`'s always-public fields) plus constructor/
    // destructor/method definitions, each of which is synthesized
    // directly into `program.functions` as an ordinary top-level
    // Function -- see ClassDef's own comment for the full reasoning and
    // the `ClassName_memberName` naming scheme used. `is_exported` (ch11
    // §11.3) marks the whole class -- and every method synthesized from
    // it -- exported as one unit, not per-member.
    //
    // ch05 §5.14: `template<typename... Ts> class Tuple;` -- a bodyless
    // forward declaration introducing a variadic generic type's own
    // primary template name. Registers the name (so `Tuple<...>` parses
    // as a type and a later specialization can reference/validate
    // against it) but pushes no real ClassDef body at all -- there is
    // nothing to instantiate directly (only a specialization, ever, is
    // -- see parse_variadic_specialization/the Monomorphizer's own
    // variadic-instantiation logic).
    [[nodiscard]] std::expected<void, ParseError> parse_variadic_primary_template_decl(Program& program, bool is_exported,
                                              std::vector<GenericTypeParam> template_params = {}) {
        SourceLocation loc = current_loc();
        if (template_params.empty()) {
            auto template_params_result = parse_generic_type_header();
            if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
            template_params = std::move(template_params_result).value();
        }
        if (auto _r = expect(TokenKind::KwClass, "'class'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto class_name_result = expect(TokenKind::Identifier, "class name");
        if (!class_name_result.has_value()) return std::unexpected(std::move(class_name_result).error());
        std::string class_name = std::string(class_name_result.value().text.data(), class_name_result.value().text.size());
        if (check(TokenKind::KwAlignas)) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'alignas' must appear before a class name, not after it (spec §9.3)"));
        }
        std::string qualified_class_name = qualify_name(class_name);
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);
        generic_type_names_.insert(qualified_class_name);
        variadic_primary_template_params_.insert_or_assign(qualified_class_name, template_params);

        ClassDef def{};

        def.loc = loc;
        def.name = qualified_class_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = template_params;
        def.template_owner_id = next_generic_template_owner_id();
        def.is_variadic_primary_template = true;
        {
            std::string _msg_6039{"class '"};
            _msg_6039 += qualified_class_name;
            _msg_6039 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_6039); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        program.classes.push_back(std::move(def));
        return {};
    }

    // ch05 §5.14: parses one of the exactly two fixed variadic
    // specialization patterns of an already-declared primary template
    // -- `template<> class Name<> { ... };` (the empty-pack base case)
    // or `template<typename Head, typename... Tail> class Name<Head,
    // Tail...> [: base] { ... };` (the recursive case). The
    // specialization's own `<...>` argument list (right after the class
    // name) must exactly restate this declaration's own template
    // header's parameter names, in order -- not a general/arbitrary
    // specialization pattern (ch05 §5.14's own scoping: "exactly two
    // fixed patterns... not arbitrary/general specialization").
    [[nodiscard]] std::expected<void, ParseError> parse_variadic_specialization(Program& program, bool is_exported) {
        SourceLocation loc = current_loc();
        auto template_params_result = parse_generic_type_header();
        if (!template_params_result.has_value()) return std::unexpected(std::move(template_params_result).error());
        std::vector<GenericTypeParam> template_params = std::move(template_params_result).value();
        if (auto _r = expect(TokenKind::KwClass, "'class'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        const Token& class_name_tok = peek();
        auto class_name_result = expect(TokenKind::Identifier, "class name");
        if (!class_name_result.has_value()) return std::unexpected(std::move(class_name_result).error());
        std::string class_name = std::string(class_name_result.value().text.data(), class_name_result.value().text.size());
        std::string qualified_class_name = qualify_name(class_name);
        auto primary_it = variadic_primary_template_params_.find(qualified_class_name);
        if (primary_it == variadic_primary_template_params_.end()) {
            {
                std::string _msg_6068{"'"};
                _msg_6068 += qualified_class_name;
                _msg_6068 += "' is not a declared variadic primary template -- a specialization requires ";
                _msg_6068 += "a preceding 'template<typename... Ts> class ";
                _msg_6068 += class_name;
                _msg_6068 += ";' forward declaration (ch05 §5.14)";
                return std::unexpected(ParseError(class_name_tok.line, class_name_tok.column,
                              _msg_6068));
            }
        }

        // The specialization's own `<...>` argument list must exactly
        // restate template_params' own names, in order (empty for the
        // base case).
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        std::size_t index = 0;
        if (!check(TokenKind::Greater)) {
            while (true) {
                if (index >= template_params.size()) {
                    const Token& tok = peek();
                    {
                        std::string _msg_6084{"this specialization's own '<...>' argument list has more entries than "};
                        _msg_6084 += "its own template header (ch05 §5.14 only supports restating the ";
                        _msg_6084 += "header's own parameter names, in order)";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_6084));
                    }
                }
                const Token& name_tok = peek();
                auto arg_name_result = expect(TokenKind::Identifier, "template parameter name");
                if (!arg_name_result.has_value()) return std::unexpected(std::move(arg_name_result).error());
                std::string arg_name = std::string(arg_name_result.value().text.data(), arg_name_result.value().text.size());
                if (arg_name != template_params[index].name) {
                    {
                        std::string _msg_6094{"expected this specialization's own template parameter '"};
                        _msg_6094 += template_params[index].name;
                        _msg_6094 += "', not '";
                        _msg_6094 += arg_name;
                        _msg_6094 += "' (ch05 §5.14 only supports restating the header's own parameter ";
                        _msg_6094 += "names, in order)";
                        return std::unexpected(ParseError(name_tok.line, name_tok.column,
                                      _msg_6094));
                    }
                }
                if (template_params[index].is_pack) { if (auto _r = expect(TokenKind::Ellipsis, "'...'"); !_r.has_value()) return std::unexpected(std::move(_r).error()); }
                index++;
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (index != template_params.size()) {
            const Token& tok = peek();
            {
                std::string _msg_6108{"this specialization's own '<...>' argument list must restate every one of its "};
                _msg_6108 += "own template header's parameters (ch05 §5.14)";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_6108));
            }
        }

        // Exactly two fixed shapes are legal (ch05 §5.14), after
        // peeling off any leading non-type parameters (e.g. TupleImpl's
        // own "Idx", always first -- ch05 §5.14's own established
        // ordering, matched here rather than supporting an arbitrary
        // interleaving parse_generic_type_header itself never
        // produces): zero remaining parameters (the empty-pack base
        // case, e.g. `TupleImpl<Idx>`) or exactly one type parameter
        // followed by a pack (the recursive case, e.g. `TupleImpl<Idx,
        // Head, Tail...>`).
        std::size_t leading_non_type_count = 0;
        while (leading_non_type_count < template_params.size() &&
               template_params[leading_non_type_count].is_non_type) {
            leading_non_type_count++;
        }
        for (std::size_t i = leading_non_type_count; i < template_params.size(); i++) {
            if (template_params[i].is_non_type) {
                const Token& tok = peek();
                {
                    std::string _msg_6130{"a variadic specialization's non-type parameter(s) must all come first, "};
                    _msg_6130 += "before any type parameter (ch05 §5.14)";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_6130));
                }
            }
        }
        std::size_t remaining = template_params.size() - leading_non_type_count;
        bool has_pack = remaining == 2 && template_params[leading_non_type_count + 1].is_pack &&
                         !template_params[leading_non_type_count].is_pack;
        if (remaining != 0 && !has_pack) {
            const Token& tok = peek();
            {
                std::string _msg_6140{"a variadic specialization's own template header must, after any leading "};
                _msg_6140 += "non-type parameter(s), be either empty (the empty-pack base case) or end in ";
                _msg_6140 += "exactly one type parameter followed by a parameter pack ('typename Head, ";
                _msg_6140 += "typename... Tail', the recursive case) (ch05 §5.14)";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_6140));
            }
        }

        // Register every one of this specialization's own template
        // parameter names as a temporary type name for the duration of
        // its own body -- mirrors parse_class_def's identical ordinary-
        // generic handling (see its own comment). A non-type
        // parameter's own name (e.g. "Idx") needs no such registration
        // (see parse_generic_function_def's identical reasoning).
        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.insert(p.name);
            class_names_.insert(p.name);
        }

        ClassDef def{};

        def.loc = loc;
        def.name = qualified_class_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = template_params;
        def.template_owner_id = next_generic_template_owner_id();
        def.is_variadic_specialization = true;
        {
            std::string _msg_6166{"class '"};
            _msg_6166 += qualified_class_name;
            _msg_6166 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_6166); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }

        // ch05 §5.14: the recursive case's own base clause, `: private
        // Tuple<Tail...>` or (with a leading non-type parameter, e.g.
        // TupleImpl) `: public TupleImpl<Idx + 1, Tail...>` -- any
        // leading non-type argument(s) are parsed as an expression
        // (evaluated later, at monomorphization time, against this
        // specialization's own concrete non-type argument -- see
        // movecheck's evaluate_non_type_arg), and the base's own final
        // argument must be exactly this specialization's own pack
        // parameter, spread whole (the only shape either of the doc's
        // own variadic examples ever needs -- see
        // BaseSpecifier::pack_arg_name's own comment).
        if (match(TokenKind::Colon)) {
            AccessSpecifier base_access = AccessSpecifier::Private;
            if (match(TokenKind::KwPublic)) {
                base_access = AccessSpecifier::Public;
            } else {
                match(TokenKind::KwPrivate);
                base_access = AccessSpecifier::Private;
            }
            const Token& base_tok = peek();
            std::string base_name = parse_qualified_name();
            if (!variadic_primary_template_params_.contains(base_name)) {
                {
                    std::string _msg_6192{"'"};
                    _msg_6192 += base_name;
                    _msg_6192 += "' is not a declared variadic primary template (ch05 §5.14)";
                    return std::unexpected(ParseError(base_tok.line, base_tok.column,
                                  _msg_6192));
                }
            }
            BaseSpecifier base = make_named_base_specifier(program, base_name, base_access);
            std::size_t base_leading_non_type_count = 0;
            // Bound to an explicit named reference first -- see the
            // identical precedent/comment in parse_unqualified_type's
            // own is_variadic branch above.
            const std::vector<GenericTypeParam>& base_variadic_params = variadic_primary_template_params_.at(base_name);
            for (const GenericTypeParam& p : base_variadic_params) {
                if (!p.is_non_type) break;
                base_leading_non_type_count++;
            }
            if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            for (std::size_t i = 0; i < base_leading_non_type_count; i++) {
                auto non_type_arg_result = parse_additive();
                if (!non_type_arg_result.has_value()) return std::unexpected(std::move(non_type_arg_result).error());
                base.base_type.non_type_args.push_back(std::shared_ptr<Expr>(std::move(non_type_arg_result).value().release()));
                if (auto _r = expect(TokenKind::Comma, "','"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            const Token& pack_tok = peek();
            auto pack_name_result = expect(TokenKind::Identifier, "the pack parameter's own name");
            if (!pack_name_result.has_value()) return std::unexpected(std::move(pack_name_result).error());
            std::string pack_name = std::string(pack_name_result.value().text.data(), pack_name_result.value().text.size());
            if (!has_pack || pack_name != template_params.back().name) {
                {
                    std::string _msg_6213{"a variadic specialization's own base class can only be instantiated by "};
                    _msg_6213 += "spreading this specialization's own pack parameter whole (e.g. '";
                    _msg_6213 += base_name;
                    _msg_6213 += "<";
                    if (has_pack) {
                        _msg_6213 += template_params.back().name;
                    } else {
                        _msg_6213 += "Tail";
                    }
                    _msg_6213 += "...>') (ch05 §5.14)";
                    return std::unexpected(ParseError(pack_tok.line, pack_tok.column,
                                  _msg_6213));
                }
            }
            if (auto _r = expect(TokenKind::Ellipsis, "'...'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            Type pack_arg = named_type(pack_name);
            pack_arg.is_pack_expansion = true;
            base.base_type.template_args.push_back(std::move(pack_arg));
            base.pack_arg_name = pack_name;
            def.base_specifiers.push_back(std::move(base));
        }

        if (auto _rv = parse_class_body_into(program, def, class_name, template_params); !_rv.has_value()) return std::unexpected(std::move(_rv).error());

        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.erase(p.name);
            class_names_.erase(p.name);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_ordinary_class_template_forward_decl(Program& program, bool is_exported,
                                                    std::vector<GenericTypeParam> template_params) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwClass, "'class'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto class_attrs_result = parse_attribute_specifier_seq();
        if (!class_attrs_result.has_value()) return std::unexpected(std::move(class_attrs_result).error());
        ParsedAttributes class_attrs = std::move(class_attrs_result).value();
        if (class_attrs.has("packed")) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "'[[scpp::packed]]' is only supported on struct/union declarations"));
        }
        if (!class_attrs.scpp_tokens.empty() || class_attrs.thread_movable_if_movable_expr != nullptr || class_attrs.thread_movable_if_shareable_expr != nullptr) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "scpp class attributes are not supported on a bodyless template forward declaration"));
        }
        auto class_name_result = expect(TokenKind::Identifier, "class name");
        if (!class_name_result.has_value()) return std::unexpected(std::move(class_name_result).error());
        std::string class_name = std::string(class_name_result.value().text.data(), class_name_result.value().text.size());
        std::string qualified_class_name = qualify_name(class_name);
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        bool saw_type = false;
        bool saw_non_type = false;
        for (const GenericTypeParam& param : template_params) {
            saw_type = saw_type || !param.is_non_type;
            saw_non_type = saw_non_type || param.is_non_type;
        }
        if (saw_type && saw_non_type) {
            {
                std::string _msg_6266{"ordinary generic classes cannot yet mix type and non-type template "};
                _msg_6266 += "parameters in one parameter list";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_6266));
            }
        }
        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);
        generic_type_names_.insert(qualified_class_name);
        ordinary_generic_type_template_params_.insert_or_assign(qualified_class_name, template_params);

        ClassDef def{};

        def.loc = loc;
        def.name = qualified_class_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.is_nodiscard = class_attrs.has_nodiscard;
        def.nodiscard_reason = class_attrs.nodiscard_reason;
        def.template_params = std::move(template_params);
        def.template_owner_id = next_generic_template_owner_id();
        def.is_forward_declaration = true;
        {
            std::string _msg_6284{"class '"};
            _msg_6284 += qualified_class_name;
            _msg_6284 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_6284); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        program.classes.push_back(std::move(def));
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_ordinary_class_partial_specialization(Program& program, bool is_exported,
                                                     std::vector<GenericTypeParam> template_params) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwClass, "'class'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        bool is_generic = !template_params.empty();
        if (is_generic) {
            for (const GenericTypeParam& param : template_params) {
                if (!param.is_non_type) {
                    struct_names_.insert(param.name);
                    class_names_.insert(param.name);
                }
            }
        }
        auto class_attrs_result = parse_attribute_specifier_seq();
        if (!class_attrs_result.has_value()) return std::unexpected(std::move(class_attrs_result).error());
        ParsedAttributes class_attrs = std::move(class_attrs_result).value();
        if (class_attrs.has("packed")) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "'[[scpp::packed]]' is only supported on struct/union declarations"));
        }
        auto class_name_result = expect(TokenKind::Identifier, "class name");
        if (!class_name_result.has_value()) return std::unexpected(std::move(class_name_result).error());
        std::string class_name = std::string(class_name_result.value().text.data(), class_name_result.value().text.size());
        std::string qualified_class_name = qualify_name(class_name);
        if (!ordinary_generic_type_template_params_.contains(qualified_class_name)) {
            {
                std::string _msg_6316{"'"};
                _msg_6316 += qualified_class_name;
                _msg_6316 += "' is not a declared ordinary generic class template -- a partial ";
                _msg_6316 += "specialization requires a preceding primary template declaration";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_6316));
            }
        }
        // Bound to an explicit named reference first -- see the range-
        // for/reborrow comment in parse_unqualified_type's is_variadic
        // branch above (same underlying reason, different function).
        const std::vector<GenericTypeParam>& primary_template_params =
            ordinary_generic_type_template_params_.at(qualified_class_name);
        for (const GenericTypeParam& param : primary_template_params) {
            if (param.is_non_type) {
                {
                    std::string _msg_6323{"ordinary partial specialization is currently only supported for class "};
                    _msg_6323 += "templates whose primary parameter list is all-type";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_6323));
                }
            }
        }
        bool saw_non_type = false;
        for (const GenericTypeParam& param : template_params) saw_non_type = saw_non_type || param.is_non_type;
        if (saw_non_type) {
            {
                std::string _msg_6331{"ordinary partial specialization is currently only supported with type template "};
                _msg_6331 += "parameters";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_6331));
            }
        }

        std::vector<Type> specialization_args{};

        std::vector<GenericTypeParam> saved_class_template_params = current_class_template_params_;
        current_class_template_params_ = template_params;
        if (auto _r = expect(TokenKind::Less, "'<'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!check(TokenKind::Greater)) {
            while (true) {
                auto arg_result = parse_template_type_argument();
                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                Type __arg_result_value = std::move(arg_result).value();
                specialization_args.push_back(std::move(__arg_result_value));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        current_class_template_params_ = std::move(saved_class_template_params);
        if (specialization_args.size() != ordinary_generic_type_template_params_.at(qualified_class_name).size()) {
            {
                std::string _msg_6352{"this partial specialization must provide exactly "};
                _msg_6352 += std::to_string(ordinary_generic_type_template_params_.at(qualified_class_name).size());
                _msg_6352 += " specialization argument(s)";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_6352));
            }
        }

        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);

        ClassDef def{};

        def.loc = loc;
        def.name = qualified_class_name;
        def.is_interface = class_attrs.has("interface");
        def.thread_movable_override = class_attrs.has("thread_movable");
        def.thread_shareable_override = class_attrs.has("thread_shareable");
        def.is_nodiscard = class_attrs.has_nodiscard;
        def.nodiscard_reason = class_attrs.nodiscard_reason;
        if (class_attrs.thread_movable_if_movable_expr != nullptr || class_attrs.thread_movable_if_shareable_expr != nullptr) {
            if (!(class_attrs.thread_movable_if_movable_expr != nullptr && class_attrs.thread_movable_if_shareable_expr != nullptr)) {
                return std::unexpected(ParseError(loc.line, loc.column,
                                 "'[[scpp::thread_movable_if(a, b)]]' requires exactly two boolean arguments"));
            }
            if (def.thread_movable_override || def.thread_shareable_override) {
            }
            def.thread_movable_if_movable_expr = std::move(class_attrs.thread_movable_if_movable_expr);
            class_attrs.thread_movable_if_movable_expr = nullptr;
            def.thread_movable_if_shareable_expr = std::move(class_attrs.thread_movable_if_shareable_expr);
            class_attrs.thread_movable_if_shareable_expr = nullptr;
        }
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = std::move(template_params);
        def.template_owner_id = next_generic_template_owner_id();
        def.is_partial_specialization = true;
        def.specialization_template_args = std::move(specialization_args);
        {
            std::string _msg_6386{"class '"};
            _msg_6386 += qualified_class_name;
            _msg_6386 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_6386); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }

        if (auto _rv = parse_named_class_base_clause(program, def.base_specifiers); !_rv.has_value()) return std::unexpected(std::move(_rv).error());

        std::vector<GenericTypeParam> def_template_params_for_body = def.template_params;
        if (auto _rv = parse_class_body_into(program, def, class_name, def_template_params_for_body); !_rv.has_value()) return std::unexpected(std::move(_rv).error());

        if (is_generic) {
            for (const GenericTypeParam& param : def.template_params) {
                if (!param.is_non_type) {
                    struct_names_.erase(param.name);
                    class_names_.erase(param.name);
                }
            }
        }
        return {};
    }

    // `template_params` (ch05 §5.14), non-empty exactly when the caller
    // (parse_top_level_item) already consumed a `template<...>` header
    // in front of `class`, additionally: registers the type parameter's
    // own bare name as a temporary type name (both struct_names_ and
    // class_names_, mirroring exactly how a concept's own witness class
    // is registered -- "T" plays the identical role here, a placeholder
    // standing in for "whatever satisfies this generic type's own
    // constraint") for the duration of this one class's body -- removed
    // again immediately afterward, since it's meaningful only within
    // this one declaration; parses each method/constructor's own
    // optional `requires ConceptName<T>` clause; and records
    // `def.template_params`/marks the synthesized ClassDef as a
    // template. The template's own methods are never themselves
    // monomorphized/checked here -- see the Monomorphizer's generic-type
    // handling (movecheck.cppm) for both the once-at-definition abstract
    // check and each concrete instantiation's own clone.
    [[nodiscard]] std::expected<void, ParseError> parse_class_def(Program& program, bool is_exported, std::vector<GenericTypeParam> template_params = {},
                         std::vector<AlignmentSpecifier> leading_alignments = {},
                         std::optional<std::string> forced_qualified_name = {},
                         bool is_local_definition = false) {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwClass, "'class'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        bool is_generic = !template_params.empty();
        if (is_generic) {
            for (const GenericTypeParam& param : template_params) {
                if (!param.is_non_type) {
                    struct_names_.insert(param.name);
                    class_names_.insert(param.name);
                }
            }
        }
        // ch05 §5.15: `class [[scpp::thread_movable]] Name { ... };` --
        // see parse_struct_def's identical handling.
        auto class_attrs_result = parse_attribute_specifier_seq();
        if (!class_attrs_result.has_value()) return std::unexpected(std::move(class_attrs_result).error());
        ParsedAttributes class_attrs = std::move(class_attrs_result).value();
        auto trailing_alignments_result = parse_alignment_specifier_seq();
        if (!trailing_alignments_result.has_value()) return std::unexpected(std::move(trailing_alignments_result).error());
        std::vector<AlignmentSpecifier> trailing_alignments = std::move(trailing_alignments_result).value();
        if (class_attrs.has("packed")) {
            return std::unexpected(ParseError(loc.line, loc.column,
                             "'[[scpp::packed]]' is only supported on struct/union declarations"));
        }
        // The bare, unqualified name as written -- used for the
        // constructor/destructor spelling checks below (`~string()`,
        // `string(...)`  inside the class body itself always use the
        // bare name, exactly like real C++, never a namespace-qualified
        // one). `qualified_class_name` (namespace_stack_-prefixed, ch11
        // §11.4) is the form used everywhere this class is *referred to*
        // from outside its own body: struct_names_/class_names_
        // registration, `this`'s declared type, and every synthesized
        // member function's own name.
        auto class_name_result = expect(TokenKind::Identifier, "class name");
        if (!class_name_result.has_value()) return std::unexpected(std::move(class_name_result).error());
        std::string class_name = std::string(class_name_result.value().text.data(), class_name_result.value().text.size());
        std::string nested_type_owner = take_pending_nested_type_owner();
        std::string qualified_class_name{};
        if (!nested_type_owner.empty()) {
            qualified_class_name = nested_type_owner;
            qualified_class_name += "::";
            qualified_class_name += class_name;
        } else {
            qualified_class_name = qualify_name(class_name);
        }
        // Register the name before parsing the body so a field/method can
        // refer to the enclosing class via a pointer, and so a
        // self-referential constructor call/access-control decision below
        // already recognizes it -- same before-parsing-the-body
        // registration order as parse_struct_def.
        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);
        if (auto _rv = register_record_tag_kind(qualified_class_name, RecordTagKind::Class, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (is_local_definition || forced_qualified_name.has_value() || !nested_type_owner.empty()) {
            if (auto _rv = register_local_type_name(class_name, qualified_class_name, loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }
        if (is_generic) {
            bool saw_type = false;
            bool saw_non_type = false;
            for (const GenericTypeParam& param : template_params) {
                saw_type = saw_type || !param.is_non_type;
                saw_non_type = saw_non_type || param.is_non_type;
            }
            if (saw_type && saw_non_type) {
                const Token& tok = peek();
                {
                    std::string _msg_6480{"ordinary generic classes cannot yet mix type and non-type template "};
                    _msg_6480 += "parameters in one parameter list";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_6480));
                }
            }
            generic_type_names_.insert(qualified_class_name);
            ordinary_generic_type_template_params_.insert_or_assign(qualified_class_name, template_params);
        }

        ClassDef def{};

        def.loc = loc;
        def.name = qualified_class_name;
        def.is_interface = class_attrs.has("interface");
        def.alignment_specs = std::move(leading_alignments);
        // std::vector::insert(pos, first, last) isn't supported by scpp's
        // self-hosting compiler yet (nor is std::make_move_iterator), so
        // append trailing_alignments onto alignment_specs one element at a
        // time instead (AlignmentSpecifier is copy-constructible, so a
        // plain copy -- rather than a move, which scpp's move-checker
        // doesn't yet support for an indexed vector element access like
        // trailing_alignments.at(i) -- is fine here; this is a small,
        // one-time list built during parsing, not a hot path).
        for (std::size_t i = 0; i < trailing_alignments.size(); i++) {
            def.alignment_specs.push_back(trailing_alignments.at(i));
        }
        def.thread_movable_override = class_attrs.has("thread_movable");
        def.thread_shareable_override = class_attrs.has("thread_shareable");
        def.is_nodiscard = class_attrs.has_nodiscard;
        def.nodiscard_reason = class_attrs.nodiscard_reason;
        if (class_attrs.thread_movable_if_movable_expr != nullptr || class_attrs.thread_movable_if_shareable_expr != nullptr) {
            if (!(class_attrs.thread_movable_if_movable_expr != nullptr && class_attrs.thread_movable_if_shareable_expr != nullptr)) {
                return std::unexpected(ParseError(loc.line, loc.column,
                                 "'[[scpp::thread_movable_if(a, b)]]' requires exactly two boolean arguments"));
            }
            if (def.thread_movable_override || def.thread_shareable_override) {
                {
                    std::string _msg_6506{"'[[scpp::thread_movable_if(a, b)]]' cannot be combined with bare "};
                    _msg_6506 += "'[[scpp::thread_movable]]' or '[[scpp::thread_shareable]]' on the same class";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_6506));
                }
            }
            def.thread_movable_if_movable_expr = std::move(class_attrs.thread_movable_if_movable_expr);
            class_attrs.thread_movable_if_movable_expr = nullptr;
            def.thread_movable_if_shareable_expr = std::move(class_attrs.thread_movable_if_shareable_expr);
            class_attrs.thread_movable_if_shareable_expr = nullptr;
        }
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported || exported_forward_class_exists(program, qualified_class_name);
        def.template_params = template_params;
        if (is_generic) def.template_owner_id = next_generic_template_owner_id();
        {
            std::string _msg_6516{"class '"};
            _msg_6516 += qualified_class_name;
            _msg_6516 += "'";
            if (auto _rv = check_export_context(program, is_exported, def.namespace_path, loc, _msg_6516); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        }

        if (match(TokenKind::Semicolon)) {
            if (is_generic) {
                {
                    std::string _msg_6521{"an ordinary bodyless forward declaration is only supported for a non-generic "};
                    _msg_6521 += "'class' in this version";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_6521));
                }
            }
            if (!def.alignment_specs.empty() || def.thread_movable_override || def.thread_shareable_override || def.thread_movable_if_movable_expr != nullptr || def.thread_movable_if_shareable_expr != nullptr || def.is_interface ||
                def.is_nodiscard) {
                {
                    std::string _msg_6528{"scpp class layout/thread/interface/nodiscard attributes are not supported on a "};
                    _msg_6528 += "bodyless forward declaration";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                 _msg_6528));
                }
            }
            def.is_forward_declaration = true;
            program.classes.push_back(std::move(def));
            return {};
        }

        // ch05 §5.14: `class Derived : public/private Base { ... };` --
        // real C++ single-inheritance syntax verbatim. `Base` must
        // already be a declared class (this parser is single-pass, same
        // requirement as every other type reference) -- a generic
        // type's own base (e.g. `Tuple<Head, Tail...> : private
        // Tuple<Tail...>`) is handled separately by the specialization
        // parser, which never reaches this ordinary path.
        if (auto _rv = parse_named_class_base_clause(program, def.base_specifiers); !_rv.has_value()) return std::unexpected(std::move(_rv).error());

        if (auto _rv = parse_class_body_into(program, def, class_name, template_params); !_rv.has_value()) return std::unexpected(std::move(_rv).error());

        if (is_generic) {
            // Un-register the temporary type-parameter name -- scoped
            // only to this one class's own declaration (see this
            // function's own comment).
            for (const GenericTypeParam& param : template_params) {
                if (!param.is_non_type) {
                    struct_names_.erase(param.name);
                    class_names_.erase(param.name);
                }
            }
        }
        return {};
    }

    // Was a local `finish_member_fn` lambda inside parse_record_body_into
    // -- see finish_out_of_line_member_definition's comment above for why
    // (a helper capturing 'this' by reference in a named variable held
    // live across parse_record_body_into's own several later,
    // mutually-exclusive branches trips the same "'this' passed by
    // mutable reference more than once" restriction once a second (or
    // third, or fourth) such named helper coexists in the same scope).
    // is_exported/generic_method_owner_id were previously captured from
    // the enclosing parse_record_body_into's own parameters of the same
    // name; now explicit parameters instead.
    void finish_member_fn(Function& fn, bool is_exported, const std::string& generic_method_owner_id) {
        fn.namespace_path = namespace_stack_;
        fn.is_exported = is_exported;
        if (!generic_method_owner_id.empty()) fn.generic_method_owner_id = generic_method_owner_id;
        // A deleted definition *is* the definition ([dcl.fct.def.delete]/1),
        // so it no more expects a later out-of-line one than `= default` does.
        fn.expects_out_of_line_definition =
            fn.body == nullptr && fn.owning_module.empty() && !fn.is_pure && !fn.is_defaulted && !fn.is_deleted;
    }

    // Was a local `enter_member_template_context` lambda inside
    // parse_record_body_into -- see finish_out_of_line_member_definition's
    // comment above for why.
    void enter_member_template_context(const std::vector<GenericTypeParam>& member_template_params) {
        for (const GenericTypeParam& p : member_template_params) {
            if (p.is_non_type) continue;
            struct_names_.insert(p.name);
            class_names_.insert(p.name);
        }
        current_function_template_params_ = member_template_params;
    }

    // Was a local `leave_member_template_context` lambda inside
    // parse_record_body_into -- see finish_out_of_line_member_definition's
    // comment above for why.
    void leave_member_template_context(const std::vector<GenericTypeParam>& member_template_params,
                                        std::vector<GenericTypeParam>& saved_function_template_params) {
        current_function_template_params_ = std::move(saved_function_template_params);
        for (const GenericTypeParam& p : member_template_params) {
            if (p.is_non_type) continue;
            struct_names_.erase(p.name);
            class_names_.erase(p.name);
        }
    }

    // Was a local `parse_member_body_or_declaration` lambda inside
    // parse_record_body_into -- see finish_out_of_line_member_definition's
    // comment above for why.
    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_member_body_or_declaration() {
        if (match(TokenKind::Semicolon)) return std::unique_ptr<Stmt>{};
        return parse_block();
    }

    // ch05 §5.14: parses a class's own `{ ... };` body (fields, access-
    // specifier sections, constructor/destructor/method definitions)
    // into `def`, then pushes it into `program.classes` -- factored out
    // of parse_class_def so parse_variadic_specialization can reuse it
    // verbatim after handling its own, differently-shaped header
    // (template header + specialization `<...>` argument list + base
    // clause) that parse_class_def's own bodyless-forward-declaration
    // sibling, parse_variadic_primary_template_decl, never reaches at
    // all. `def`'s own name/namespace_path/is_exported/template_params/
    // base_specifiers/is_variadic_specialization must
    // already be set by the caller; only `fields` is populated here.
    // Parses one member type definition -- a `struct`, `class`,
    // `union`, or `enum class` declared inside another type's body
    // ([class.mem]). SCPP26 restricts neither: ch11 §11.1(4) says rules
    // (2) and (3) "do not otherwise restrict a `struct`", ch11 §11.1(5)
    // itself writes a rule about "a member function of a nested class",
    // and the erasure model (§3.1/Clause 4) adopts the ordinary C++
    // rules for everything this document does not modify.
    //
    // The member type is *not* stored in the enclosing type. It is
    // parsed straight into `program` under the qualified name
    // `Outer::Inner`, which is the same `A::B` shape a namespace-scope
    // type already carries -- so layout, mangling, codegen and module
    // serialization need no change to handle one, and `Outer::Inner`
    // resolves from outside through the qualified branch of
    // resolve_visible_type_name that already serves namespaces. Its bare
    // name is registered in the enclosing body's scope frame (pushed by
    // parse_record_body_into) so the rest of that body can say `Inner`.
    [[nodiscard]] std::expected<void, ParseError> parse_member_type_definition(
        Program& program, const std::string& qualified_owner_name,
        const std::vector<GenericTypeParam>& owner_template_params, const SourceLocation& member_loc,
        bool member_is_template, bool member_is_static, bool member_is_virtual, bool member_is_explicit,
        bool member_requested_unsafe, bool member_requested_nodiscard, bool member_has_eval_mode,
        bool owner_is_exported, std::vector<AlignmentSpecifier> member_alignments) {
        if (member_is_template) {
            return std::unexpected(ParseError(member_loc.line, member_loc.column,
                             "a member type definition cannot be a member template in this version"));
        }
        if (!owner_template_params.empty()) {
            return std::unexpected(ParseError(member_loc.line, member_loc.column,
                             "a member type definition inside a generic type is not supported in this version (the member type would have to be instantiated once per enclosing specialization)"));
        }
        if (member_is_static || member_is_virtual || member_is_explicit || member_has_eval_mode) {
            return std::unexpected(ParseError(member_loc.line, member_loc.column,
                             "a member type definition cannot carry 'static', 'virtual', 'explicit', 'constexpr' or 'consteval'"));
        }
        if (member_requested_unsafe || member_requested_nodiscard) {
            return std::unexpected(ParseError(member_loc.line, member_loc.column,
                             "an attribute on a member type definition belongs after its class-key, as in 'struct [[scpp::packed]] Inner { ... };'"));
        }
        if (check(TokenKind::KwEnum)) {
            if (auto _rv = reject_alignment_specifiers(member_alignments, "an 'enum class' declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            pending_nested_type_owner_ = qualified_owner_name;
            auto enum_def_result = parse_enum_def();
            if (!enum_def_result.has_value()) {
                pending_nested_type_owner_.clear();
                return std::unexpected(std::move(enum_def_result).error());
            }
            EnumDef enum_def = std::move(enum_def_result).value();
            // ch11 §11.3: exporting a type exports its whole member
            // surface as one unit, so a member type follows its owner.
            enum_def.is_exported = owner_is_exported;
            program.enums.push_back(std::move(enum_def));
            return {};
        }
        if (check(TokenKind::KwUnion)) {
            pending_nested_type_owner_ = qualified_owner_name;
            auto union_def_result = parse_union_def(std::move(member_alignments));
            if (!union_def_result.has_value()) {
                pending_nested_type_owner_.clear();
                return std::unexpected(std::move(union_def_result).error());
            }
            StructDef union_def = std::move(union_def_result).value();
            union_def.is_exported = owner_is_exported;
            program.structs.push_back(std::move(union_def));
            return {};
        }
        std::vector<GenericTypeParam> no_template_params{};
        std::optional<std::string> no_forced_name{};
        if (check(TokenKind::KwStruct)) {
            pending_nested_type_owner_ = qualified_owner_name;
            auto struct_result = parse_struct_def(program, owner_is_exported, std::move(no_template_params),
                                                  std::move(member_alignments), std::move(no_forced_name), false);
            if (!struct_result.has_value()) {
                pending_nested_type_owner_.clear();
                return std::unexpected(std::move(struct_result).error());
            }
            StructDef __struct_result_value = std::move(struct_result).value();
            program.structs.push_back(std::move(__struct_result_value));
            return {};
        }
        pending_nested_type_owner_ = qualified_owner_name;
        // parse_class_def pushes the finished ClassDef into `program`
        // itself, at the end of its own parse_class_body_into call.
        if (auto _rv = parse_class_def(program, owner_is_exported, std::move(no_template_params),
                                       std::move(member_alignments), std::move(no_forced_name), false);
            !_rv.has_value()) {
            pending_nested_type_owner_.clear();
            return std::unexpected(std::move(_rv).error());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ParseError> parse_record_body_into(Program& program, const std::string& owner_name, const std::string& qualified_owner_name,
                                const std::string& synthesized_member_owner_name,
                                const std::vector<GenericTypeParam>& template_params, bool is_exported,
                                const std::string& generic_method_owner_id, AccessSpecifier default_access,
                                bool allow_using_declarations, bool allow_virtual_members,
                                std::string_view owner_keyword, std::string_view member_decl_context,
                                std::string_view virtual_member_error, const RecordUsingHandlerFn& handle_using,
                                const RecordFieldAdderFn& add_field) {
        // handle_using/add_field stay const-ref parameters (codegen's
        // overload resolution can only match a bare lambda-literal
        // argument against a *reference*-typed std::function parameter
        // via its own materialized-temporary path -- passing them
        // by value instead makes this and every call site fail to
        // resolve at all, "call to unknown function ... (resolve)");
        // but std::function's `call()` needs a non-const receiver (see
        // RecordUsingHandlerFn's own declaration comment), so this
        // makes one local, mutable copy of each up front (cheap: each
        // is just a few pointers) and calls through that instead of the
        // const-ref parameter directly.
        RecordUsingHandlerFn handle_using_fn = handle_using;
        RecordFieldAdderFn add_field_fn = add_field;
        // Every method/constructor/destructor synthesized below shares
        // this same namespace_path/is_exported (ch11 §11.3: exporting a
        // type exports its whole member surface as one unit) --
        // owning_module stays default-empty (this program's own
        // declaration; only set later, at cross-module merge time).
        std::vector<GenericTypeParam> saved_class_template_params = current_class_template_params_;
        current_class_template_params_ = template_params;
        if (!template_params.empty()) {
            injected_generic_type_name_stack_.push_back(
                InjectedGenericTypeName{owner_name, qualified_owner_name, template_params});
        }

        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        // A member type's bare name is visible to the rest of the
        // enclosing body ([basic.scope.class]), so the body gets its own
        // frame in the same scope stack local types already use --
        // register_local_type_name writes into the innermost frame, and
        // resolve_visible_local_type_name searches innermost-first, which
        // is what makes an inner member type shadow an outer one of the
        // same name. Popped at the single success exit below; an early
        // return here abandons the whole parse anyway (same as
        // parse_block's own push/pop).
        local_type_name_scopes_.emplace_back();
        AccessSpecifier current_access = default_access;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::KwPublic)) {
                if (auto _r = expect(TokenKind::Colon, "':'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                current_access = AccessSpecifier::Public;
                continue;
            }
            if (match(TokenKind::KwPrivate)) {
                if (auto _r = expect(TokenKind::Colon, "':'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                current_access = AccessSpecifier::Private;
                continue;
            }

            SourceLocation member_loc = current_loc();
            bool member_is_template = false;
            std::vector<GenericTypeParam> member_template_params{};

            std::vector<GenericTypeParam> saved_function_template_params = current_function_template_params_;
            if (check(TokenKind::KwTemplate)) {
                auto member_template_params_result = parse_generic_type_header();
                if (!member_template_params_result.has_value()) return std::unexpected(std::move(member_template_params_result).error());
                member_template_params = std::move(member_template_params_result).value();
                member_is_template = true;
                enter_member_template_context(member_template_params);
            }
            // ch01 §1.2/§1.3: `[[scpp::unsafe]]` on a method/constructor/
            // destructor's own declaration -- identical function-level
            // marker semantics as a free function (see parse_function's
            // own handling), just reached from inside a type body
            // instead. Parsed once here, before dispatching on which
            // member shape follows; rejected if it turns out to be a
            // field (unsafe is only ever meaningful on a function, ch01
            // §1.3 (1)).
            auto member_alignments_result = parse_alignment_specifier_seq();
            if (!member_alignments_result.has_value()) return std::unexpected(std::move(member_alignments_result).error());
            std::vector<AlignmentSpecifier> member_alignments = std::move(member_alignments_result).value();
            const Token& member_attr_start_tok = peek();
            auto member_attrs_result = parse_attribute_specifier_seq();
            if (!member_attrs_result.has_value()) return std::unexpected(std::move(member_attrs_result).error());
            ParsedAttributes member_attrs = std::move(member_attrs_result).value();
            if (auto _rv = reject_packed_attribute(member_attrs, member_attr_start_tok, member_decl_context.data()); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            bool member_requested_unsafe = member_attrs.has("unsafe");
            bool member_requested_nodiscard = member_attrs.has_nodiscard;
            std::string member_nodiscard_reason = member_attrs.nodiscard_reason;
            bool member_is_static = false;
            bool member_is_virtual = false;
            bool member_is_explicit = false;
            FunctionEvalMode member_eval_mode = FunctionEvalMode::RuntimeOnly;
            for (;;) {
                if (match(TokenKind::KwStatic)) {
                    member_is_static = true;
                    continue;
                }
                if (match(TokenKind::KwVirtual)) {
                    member_is_virtual = true;
                    continue;
                }
                if (match(TokenKind::KwExplicit)) {
                    member_is_explicit = true;
                    continue;
                }
                if (member_eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConstexpr)) {
                    advance();
                    member_eval_mode = FunctionEvalMode::Constexpr;
                    continue;
                }
                if (member_eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConsteval)) {
                    advance();
                    member_eval_mode = FunctionEvalMode::Consteval;
                    continue;
                }
                if ((check(TokenKind::KwConstexpr) || check(TokenKind::KwConsteval)) &&
                    member_eval_mode != FunctionEvalMode::RuntimeOnly) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column,
                                     "a declaration may specify at most one of 'constexpr' or 'consteval'"));
                }
                break;
            }
            if (member_is_virtual && !allow_virtual_members) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column, std::string(virtual_member_error.data(), virtual_member_error.size())));
            }
            if (member_is_explicit && !(check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == owner_name &&
                                        peek_at(1).kind == TokenKind::LParen)) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                 "'explicit' is only allowed directly before a constructor declaration"));
            }
            if (check(TokenKind::KwStruct) || check(TokenKind::KwClass) || check(TokenKind::KwEnum) ||
                check(TokenKind::KwUnion)) {
                if (auto _rv = parse_member_type_definition(program, qualified_owner_name, template_params, member_loc,
                                                            member_is_template, member_is_static, member_is_virtual,
                                                            member_is_explicit, member_requested_unsafe,
                                                            member_requested_nodiscard,
                                                            member_eval_mode != FunctionEvalMode::RuntimeOnly,
                                                            is_exported, member_alignments);
                    !_rv.has_value()) {
                    return std::unexpected(std::move(_rv).error());
                }
                current_function_template_params_ = std::move(saved_function_template_params);
                continue;
            }
            if (check(TokenKind::KwUsing)) {
                if (!allow_using_declarations) {
                    {
                        std::string _msg_6717{"a declaration introduced by '"};
                        _msg_6717 += std::string(owner_keyword.data(), owner_keyword.size());
                        _msg_6717 += "' shall not declare a class-scope using declaration because a ";
                        _msg_6717 += std::string(owner_keyword.data(), owner_keyword.size());
                        _msg_6717 += " has no base-clause in this version";
                        return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     _msg_6717));
                    }
                }
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     "a class-scope using declaration cannot be a member template"));
                }
                if (member_requested_unsafe || member_requested_nodiscard || member_is_static || member_is_virtual ||
                    member_eval_mode != FunctionEvalMode::RuntimeOnly) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     "a class-scope using declaration cannot carry function specifiers or attributes"));
                }
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a class-scope using declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (!handle_using_fn(current_access)) {
                    ParseError _using_err{pending_using_error_.value()};
                    return std::unexpected(std::move(_using_err));
                }
                continue;
            }
            if (match(TokenKind::Tilde)) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a destructor declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a destructor cannot be a member template"));
                }
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a destructor cannot be declared static"));
                }
                if (member_eval_mode != FunctionEvalMode::RuntimeOnly) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a destructor cannot be declared constexpr or consteval"));
                }
                auto name_tok_result = expect(TokenKind::Identifier, "destructor name");
                if (!name_tok_result.has_value()) return std::unexpected(std::move(name_tok_result).error());
                const Token& name_tok = name_tok_result.value();
                if (name_tok.text != owner_name) {
                    {
                        std::string _msg_6753{"destructor name '~"};
                        _msg_6753 += std::string(name_tok.text.data(), name_tok.text.size());
                        _msg_6753 += "' must match the enclosing ";
                        _msg_6753 += std::string(owner_keyword.data(), owner_keyword.size());
                        _msg_6753 += " name '";
                        _msg_6753 += owner_name;
                        _msg_6753 += "'";
                        return std::unexpected(ParseError(name_tok.line, name_tok.column,
                                      _msg_6753));
                    }
                }
                if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.member_owner_class = qualified_owner_name;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = synthesized_member_owner_name;
                fn.name += "_delete";
                fn.params.push_back(make_this_param(qualified_owner_name, /*is_const=*/false));
                auto fn_body_result = parse_member_function_suffix(fn);
                if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                fn.body = std::move(fn_body_result).value();
                if (auto _rv = validate_defaulted_special_member(fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == owner_name &&
                peek_at(1).kind == TokenKind::LParen) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a constructor declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                advance(); // owner name
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a constructor cannot be declared static"));
                }
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = synthesized_member_owner_name;
                fn.name += "_new";
                auto fn_params_result = parse_param_list(/*allow_unnamed_single_parameter=*/true);
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = parse_function_trailing_attributes(fn, "a constructor declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_generic_params(fn.params, "a constructor"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                fn.template_params = member_template_params;
                fn.is_generic_template = member_is_template;
                fn.params = prepend_this_param(fn.params, qualified_owner_name, /*is_const=*/false);
                fn.is_override = match(TokenKind::KwOverride);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                if (!check(TokenKind::Semicolon) && !check(TokenKind::Assign)) {
                    auto fn_member_initializers_result = parse_constructor_member_initializer_list();
                    if (!fn_member_initializers_result.has_value()) return std::unexpected(std::move(fn_member_initializers_result).error());
                    fn.member_initializers = std::move(fn_member_initializers_result).value();
                }
                if (match(TokenKind::Assign)) {
                    if (auto _rv = parse_deleted_defaulted_or_pure_suffix(fn, /*allow_default=*/true, /*allow_pure=*/true,
                                                                          "a member declaration");
                        !_rv.has_value()) {
                        return std::unexpected(std::move(_rv).error());
                    }
                } else {
                    auto fn_body_result = parse_member_body_or_declaration();
                    if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                    fn.body = std::move(fn_body_result).value();
                }
                if (auto _rv = reject_unnamed_defaulted_single_param_if_needed(fn.params, fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = validate_defaulted_special_member(fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (is_move_constructor_function(fn) && !fn.is_defaulted) {
                    {
                        std::string _msg_6845{"a move constructor cannot be user-declared for "};
                        _msg_6845 += std::string(owner_keyword.data(), owner_keyword.size());
                        _msg_6845 += " '";
                        _msg_6845 += owner_name;
                        _msg_6845 += "' -- the compiler always provides one (spec ";
                        _msg_6845 += "§6.4(1)/(2), ch04 §4.2)";
                        return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      _msg_6845));
                    }
                }
                if (fn.body == nullptr && !fn.member_initializers.empty()) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     "a constructor member-initializer-list is only allowed on a constructor definition"));
                }
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }

            auto member_type_result = parse_type();

            if (!member_type_result.has_value()) return std::unexpected(std::move(member_type_result).error());

            Type member_type = std::move(member_type_result).value();
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator" &&
                peek_at(1).kind == TokenKind::Star) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a member function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator* cannot currently be a member template"));
                }
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator* cannot be declared static"));
                }
                advance(); // 'operator'
                advance(); // '*'
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                auto fn_params_result = parse_param_list();
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_generic_params(fn.params, "an operator*"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                bool is_const = match(TokenKind::KwConst);
                fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
                fn.return_type = std::move(member_type);
                fn.name = synthesized_member_owner_name;
                fn.name += "_operator_deref";
                fn.params = prepend_this_param(fn.params, qualified_owner_name, is_const);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                auto fn_body_result = parse_member_function_suffix(fn);
                if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                fn.body = std::move(fn_body_result).value();
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator" &&
                peek_at(1).kind == TokenKind::Arrow) {
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator-> cannot currently be a member template"));
                }
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator-> cannot be declared static"));
                }
                advance(); // 'operator'
                advance(); // '->'
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                auto fn_params_result = parse_param_list();
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_generic_params(fn.params, "an operator->"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                bool is_const = match(TokenKind::KwConst);
                fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
                fn.return_type = std::move(member_type);
                fn.name = synthesized_member_owner_name;
                fn.name += "_operator_arrow";
                fn.params = prepend_this_param(fn.params, qualified_owner_name, is_const);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                auto fn_body_result = parse_member_function_suffix(fn);
                if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                fn.body = std::move(fn_body_result).value();
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator" &&
                (peek_at(1).kind == TokenKind::EqualEqual || peek_at(1).kind == TokenKind::NotEqual)) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a member function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                advance(); // 'operator'
                TokenKind operator_kind = advance().kind;
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.is_static = member_is_static;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                auto fn_params_result = parse_param_list(/*allow_unnamed_single_parameter=*/true);
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_generic_params(fn.params, operator_kind == TokenKind::EqualEqual ? "an operator==" : "an operator!="); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                fn.template_params = member_template_params;
                fn.is_generic_template = member_is_template;
                bool is_const = match(TokenKind::KwConst);
                fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
                if (member_is_static && (is_const || fn.receiver_ref_qualifier != ReceiverRefQualifier::None)) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     "a static member function cannot be const-qualified or ref-qualified"));
                }
                fn.return_type = std::move(member_type);
                fn.name = synthesized_member_owner_name;
                fn.name += std::string(operator_kind == TokenKind::EqualEqual ? "_operator_equal" : "_operator_not_equal");
                if (!member_is_static) {
                    fn.params = prepend_this_param(fn.params, qualified_owner_name, is_const);
                }
                fn.is_override = match(TokenKind::KwOverride);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                auto fn_body_result = parse_member_function_suffix(fn);
                if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                fn.body = std::move(fn_body_result).value();
                if (auto _rv = reject_unnamed_defaulted_single_param_if_needed(fn.params, fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = validate_defaulted_special_member(fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator" &&
                peek_at(1).kind == TokenKind::Assign) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a member function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator= cannot currently be a member template"));
                }
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "an operator= cannot be declared static"));
                }
                advance(); // 'operator'
                advance(); // '='
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                auto fn_params_result = parse_param_list(/*allow_unnamed_single_parameter=*/true);
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = reject_generic_params(fn.params, "an operator="); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                bool is_const = match(TokenKind::KwConst);
                fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
                fn.return_type = std::move(member_type);
                fn.name = synthesized_member_owner_name;
                fn.name += "_operator_assign";
                fn.params = prepend_this_param(fn.params, qualified_owner_name, is_const);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                auto fn_body_result = parse_member_function_suffix(fn);
                if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                fn.body = std::move(fn_body_result).value();
                if (auto _rv = reject_unnamed_defaulted_single_param_if_needed(fn.params, fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (auto _rv = validate_defaulted_special_member(fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                if (is_move_assignment_function(fn) && !fn.is_defaulted) {
                    {
                        std::string _msg_7040{"a move assignment operator cannot be user-declared for "};
                        _msg_7040 += std::string(owner_keyword.data(), owner_keyword.size());
                        _msg_7040 += " '";
                        _msg_7040 += owner_name;
                        _msg_7040 += "' -- the compiler always provides one (spec ";
                        _msg_7040 += "§6.4(1)/(2), ch04 §4.2)";
                        return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      _msg_7040));
                    }
                }
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (starts_function_pointer_declarator()) {
                if (member_is_template) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a member template declaration must declare a constructor or method, not a field"));
                }
                if (member_eval_mode != FunctionEvalMode::RuntimeOnly) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "only a member function or constructor may be declared constexpr or consteval"));
                }
                if (member_requested_unsafe) {
                    {
                        std::string _msg_7060{"'[[scpp::unsafe]]' cannot appertain to a member variable -- only to a "};
                        _msg_7060 += "compound-statement or a function's own declaration (ch01 §1.3)";
                        return std::unexpected(ParseError(member_attr_start_tok.line, member_attr_start_tok.column,
                                      _msg_7060));
                    }
                }
                if (member_is_static) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "static data members are not supported in this version"));
                }
                if (member_is_virtual) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                     "a member variable cannot be declared virtual"));
                }
                std::string field_name{};

                auto field_type_result = parse_function_pointer_declarator(std::move(member_type), field_name);
                if (!field_type_result.has_value()) return std::unexpected(std::move(field_type_result).error());
                Type field_type = std::move(field_type_result).value();
                if (check(TokenKind::Colon)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column, "bit-field declarations are not supported in this version"));
                }
                auto default_initializer_result = parse_optional_default_initializer(std::string(member_decl_context.data(), member_decl_context.size()));
                if (!default_initializer_result.has_value()) return std::unexpected(std::move(default_initializer_result).error());
                std::optional<Initializer> default_initializer = std::move(default_initializer_result).value();
                if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                add_field_fn(member_loc, std::move(field_type), std::move(field_name), current_access, std::move(default_initializer),
                          std::move(member_alignments));
                continue;
            }
            auto member_name_tok_result = expect(TokenKind::Identifier, "field or method name");
            if (!member_name_tok_result.has_value()) return std::unexpected(std::move(member_name_tok_result).error());
            std::string member_name = std::string(member_name_tok_result.value().text.data(), member_name_tok_result.value().text.size());
            if (check(TokenKind::LParen)) {
                if (auto _rv = reject_alignment_specifiers(member_alignments, "a member function declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                Function fn{};

                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.is_nodiscard = member_requested_nodiscard;
                fn.nodiscard_reason = member_nodiscard_reason;
                fn.eval_mode = member_eval_mode;
                fn.member_owner_class = qualified_owner_name;
                fn.is_static = member_is_static;
                fn.access = current_access;
                fn.is_virtual = member_is_virtual;
                auto fn_params_result = parse_param_list();
                if (!fn_params_result.has_value()) return std::unexpected(std::move(fn_params_result).error());
                fn.params = std::move(fn_params_result).value();
                if (auto _rv = reject_generic_params(fn.params, "a method"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                fn.template_params = member_template_params;
                fn.is_generic_template = member_is_template;
                bool is_const = match(TokenKind::KwConst);
                if (auto _rv = parse_function_trailing_attributes(fn, "a member function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                fn.receiver_ref_qualifier = parse_optional_ref_qualifier();
                if (member_is_static && (is_const || fn.receiver_ref_qualifier != ReceiverRefQualifier::None)) {
                    return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                      "a static member function cannot be const-qualified or ref-qualified"));
                }
                fn.return_type = std::move(member_type);
                fn.name = synthesized_member_owner_name;
                fn.name += "_";
                fn.name += member_name;
                if (!member_is_static) {
                    fn.params = prepend_this_param(fn.params, qualified_owner_name, is_const);
                }
                fn.is_override = match(TokenKind::KwOverride);
                auto fn_requires_result = parse_optional_method_requires_clause(template_params);
                if (!fn_requires_result.has_value()) return std::unexpected(std::move(fn_requires_result).error());
                fn.method_requires_concept = std::move(fn_requires_result).value();
                if (match(TokenKind::Assign)) {
                    if (auto _rv = parse_deleted_defaulted_or_pure_suffix(fn, /*allow_default=*/true, /*allow_pure=*/true,
                                                                          "a member declaration");
                        !_rv.has_value()) {
                        return std::unexpected(std::move(_rv).error());
                    }
                } else if (match(TokenKind::Semicolon)) {
                    fn.body = nullptr;
                } else {
                    auto fn_body_result = parse_block();
                    if (!fn_body_result.has_value()) return std::unexpected(std::move(fn_body_result).error());
                    fn.body = std::move(fn_body_result).value();
                }
                if (auto _rv = validate_defaulted_special_member(fn, member_loc); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                finish_member_fn(fn, is_exported, generic_method_owner_id);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }

            if (member_eval_mode != FunctionEvalMode::RuntimeOnly) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                  "only a member function or constructor may be declared constexpr or consteval"));
            }
            if (member_requested_unsafe) {
                {
                    std::string _msg_7163{"'[[scpp::unsafe]]' cannot appertain to a member variable -- only to a "};
                    _msg_7163 += "compound-statement or a function's own declaration (ch01 §1.3)";
                    return std::unexpected(ParseError(member_attr_start_tok.line, member_attr_start_tok.column,
                                  _msg_7163));
                }
            }
            if (member_is_static) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                  "static data members are not supported in this version"));
            }
            if (member_is_virtual) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                 "a member variable cannot be declared virtual"));
            }
            if (member_is_template) {
                return std::unexpected(ParseError(member_loc.line, member_loc.column,
                                  "a member template declaration must declare a constructor or method, not a field"));
            }
            if (check(TokenKind::Colon)) {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column, "bit-field declarations are not supported in this version"));
            }
            auto field_type_result = parse_array_suffix(std::move(member_type));
            if (!field_type_result.has_value()) return std::unexpected(std::move(field_type_result).error());
            Type field_type = std::move(field_type_result).value();
            auto default_initializer_result = parse_optional_default_initializer(std::string(member_decl_context.data(), member_decl_context.size()));
            if (!default_initializer_result.has_value()) return std::unexpected(std::move(default_initializer_result).error());
            std::optional<Initializer> default_initializer = std::move(default_initializer_result).value();
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            add_field_fn(member_loc, std::move(field_type), std::move(member_name), current_access, std::move(default_initializer),
                      std::move(member_alignments));
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        local_type_name_scopes_.pop_back();
        if (!template_params.empty()) injected_generic_type_name_stack_.pop_back();
        current_class_template_params_ = std::move(saved_class_template_params);
        std::expected<void, ParseError> body_ok{};
        return body_ok;
    }

    [[nodiscard]] std::expected<void, ParseError> parse_class_body_into(Program& program, ClassDef& def, const std::string& class_name,
                                const std::vector<GenericTypeParam>& template_params) {
        std::string qualified_class_name = def.name;
        std::string synthesized_member_owner_name = qualified_class_name;
        if ((def.is_partial_specialization || def.is_variadic_specialization) && !def.template_owner_id.empty()) {
            synthesized_member_owner_name += "__";
            synthesized_member_owner_name += def.template_owner_id;
        }
        // Wrap each lambda literal in an explicit, fully-qualified
        // std::function<Sig>(...) constructor-call at the argument
        // position itself -- see the matching comment at the struct
        // call site (parse_struct_or_class_def) above for the full
        // rationale (avoids both a codegen gap for bare lambda-literal
        // arguments, and a `this`-borrow conflict that would arise
        // from binding a this-capturing closure to a named local
        // before this call, which is itself a Parser method needing
        // its own implicit `this` receiver borrow).
        // The using-declaration callback below needs explicit 'this' too
        // (calls parse_class_using_declaration, a Parser method) -- see
        // try_finish's comment above. The field adder calls no Parser
        // method, so it captures nothing but the enclosing locals.
        if (auto _rv = parse_record_body_into(
                program, class_name, qualified_class_name, synthesized_member_owner_name, template_params, def.is_exported,
                def.template_owner_id, AccessSpecifier::Private,
                /*allow_using_declarations=*/true, /*allow_virtual_members=*/true, "class", "a class member declaration",
                /*virtual_member_error=*/"",
                std::function<bool(AccessSpecifier)>([&, this](AccessSpecifier access) -> bool {
                    auto _r = parse_class_using_declaration(def, access);
                    if (!_r.has_value()) {
                        pending_using_error_ = std::move(_r).error();
                        return false;
                    }
                    return true;
                }),
                // By-const-ref parameters (see RecordFieldAdderFn's own
                // declaration comment above) mean this copies rather than
                // moves into the new ClassField.
                std::function<void(const SourceLocation&, const Type&, const std::string&, AccessSpecifier,
                                    const std::optional<Initializer>&, const std::vector<AlignmentSpecifier>&)>(
                    [&](const SourceLocation& field_loc, const Type& field_type, const std::string& field_name, AccessSpecifier access,
                        const std::optional<Initializer>& default_initializer, const std::vector<AlignmentSpecifier>& alignment_specs) {
                        ClassField field{};

                        field.loc = field_loc;
                        field.type = field_type;
                        field.name = field_name;
                        field.access = access;
                        field.default_initializer = default_initializer;
                        field.alignment_specs = alignment_specs;
                        def.fields.push_back(std::move(field));
                    }));
            !_rv.has_value()) {
            return std::unexpected(std::move(_rv).error());
        }
        program.classes.push_back(std::move(def));
        return std::expected<void, ParseError>{};
    }

    // Parses one function declaration or definition's `<return-type>
    // <name>(<params>)` followed by either `;` (a bodyless declaration --
    // legal for `extern "C"` (ch02 §2.1), bare `extern` (ch11 §11.6), or
    // an ordinary forward declaration later reconciled against a matching
    // definition in the same translation unit) or `{ <body> }` (an
    // ordinary definition).
    // `is_extern_c`/`is_module_extern` are decided and consumed by the
    // caller (parse_top_level_function_or_extern_group) before this
    // runs, since their combination affects *which* prefixes were
    // already consumed (an item inside an `extern "C" { }` block isn't
    // preceded by its own `extern "C"` -- see that function).
    [[nodiscard]] std::expected<Function, ParseError> parse_function(bool is_extern_c, bool is_module_extern = false, bool is_unsafe = false,
                            bool is_nodiscard = false, const std::string& nodiscard_reason = {}) {
        Function fn{};

        fn.loc = current_loc();
        fn.is_extern_c = is_extern_c;
        fn.is_module_extern = is_module_extern;
        fn.is_unsafe = is_unsafe;
        fn.is_nodiscard = is_nodiscard;
        fn.nodiscard_reason = nodiscard_reason;
        bool saw_inline = false;
        bool saw_default_argument = false;
        for (;;) {
            if (!saw_inline && match(TokenKind::KwInline)) {
                saw_inline = true;
                continue;
            }
            if (fn.eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConstexpr)) {
                advance();
                fn.eval_mode = FunctionEvalMode::Constexpr;
                continue;
            }
            if (fn.eval_mode == FunctionEvalMode::RuntimeOnly && check(TokenKind::KwConsteval)) {
                advance();
                fn.eval_mode = FunctionEvalMode::Consteval;
                continue;
            }
            if ((check(TokenKind::KwConstexpr) || check(TokenKind::KwConsteval)) &&
                fn.eval_mode != FunctionEvalMode::RuntimeOnly) {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column,
                                 "a declaration may specify at most one of 'constexpr' or 'consteval'"));
            }
            break;
        }
        auto return_type_result = parse_type_with_lifetime_attributes_enabled();
        if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
        fn.return_type = std::move(return_type_result).value();
        auto fn_name_result = expect(TokenKind::Identifier, "function name");
        if (!fn_name_result.has_value()) return std::unexpected(std::move(fn_name_result).error());
        fn.name = std::string(fn_name_result.value().text.data(), fn_name_result.value().text.size());

        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (!check(TokenKind::RParen)) {
            while (true) {
                if (match(TokenKind::Ellipsis)) {
                    // `...` must be the last thing in the parameter list
                    // (as in real C++) -- anything after it is left
                    // unconsumed, so the expect(RParen) below reports a
                    // clear parse error for e.g. `(..., int x)`.
                    fn.has_varargs = true;
                    break;
                }
                Param param{};
                param.loc = SourceLocation{peek().line, peek().column};

                Type base_type{};

                {
                    auto base_type_result = parse_param_type_with_lifetime_attributes_enabled(param.generic_concept);
                    if (!base_type_result.has_value()) return std::unexpected(std::move(base_type_result).error());
                    base_type = std::move(base_type_result).value();
                }
                param.is_parameter_pack = match(TokenKind::Ellipsis);
                if (param.is_parameter_pack && param.generic_concept.empty() &&
                    !referenced_pack_type_param_name(base_type).has_value()) {
                    const Token& tok = peek();
                    {
                        std::string _msg_7312{"parameter packs are only supported for the abbreviated generic form "};
                        _msg_7312 += "('Concept auto&... args') in this version (ch05 §5.11)";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                      _msg_7312));
                    }
                }
                auto param_type_result = parse_named_declarator(std::move(base_type), param.name, "parameter name");
                if (!param_type_result.has_value()) return std::unexpected(std::move(param_type_result).error());
                Type param_type = std::move(param_type_result).value();
                if (param.is_parameter_pack && !check(TokenKind::RParen)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column,
                                      "a parameter pack must be the last parameter in the list (ch05 §5.11)"));
                }
                set_param_declared_type(param, std::move(param_type));
                // ch05 §5.15: see parse_param_list's identical trailing-
                // attribute handling -- this is the separate parameter-
                // parsing loop parse_function itself uses (top-level
                // ordinary/generic/extern functions), not shared with
                // parse_param_list (class methods/lambdas).
                const Token& param_attr_start_tok = peek();
                auto param_attrs_result = parse_attribute_specifier_seq();
                if (!param_attrs_result.has_value()) return std::unexpected(std::move(param_attrs_result).error());
                ParsedAttributes param_attrs = std::move(param_attrs_result).value();
                if (auto _rv = reject_packed_attribute(param_attrs, param_attr_start_tok, "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                param.require_thread_movable = param_attrs.has("thread_movable");
                param.require_thread_shareable = param_attrs.has("thread_shareable");
                if (auto _rv = merge_lifetime_attribute(param.lifetime, param_attrs.lifetime, param_attr_start_tok,
                                         "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                LifetimeAnnotation param_lifetime_for_hoist = param.lifetime;
                if (auto _rv = hoist_type_lifetime_annotation(param.type, param_lifetime_for_hoist, param_attr_start_tok, "a parameter declaration"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                param.lifetime = std::move(param_lifetime_for_hoist);
                if (match(TokenKind::Assign)) {
                    if (param.is_parameter_pack) {
                        const Token& tok = peek();
                        return std::unexpected(ParseError(tok.line, tok.column, "a parameter pack cannot have a default argument"));
                    }
                    auto default_expr_result = parse_default_argument_expr(param.type);
                    if (!default_expr_result.has_value()) return std::unexpected(std::move(default_expr_result).error());
                    param.default_expr = std::shared_ptr<Expr>(std::move(default_expr_result).value().release());
                    saw_default_argument = true;
                } else if (saw_default_argument) {
                    const Token& tok = peek();
                    {
                        std::string _msg_7351{"once a parameter has a default argument, every later parameter must also "};
                        _msg_7351 += "have one";
                        return std::unexpected(ParseError(tok.line, tok.column,
                                     _msg_7351));
                    }
                }
                fn.params.push_back(std::move(param));
                if (!(match(TokenKind::Comma))) break;
            }
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        const Token& fn_attr_start_tok = peek();
        auto fn_attrs_result = parse_attribute_specifier_seq();
        if (!fn_attrs_result.has_value()) return std::unexpected(std::move(fn_attrs_result).error());
        ParsedAttributes fn_attrs = std::move(fn_attrs_result).value();
        if (auto _rv = reject_packed_attribute(fn_attrs, fn_attr_start_tok, "a function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        if (auto _rv = merge_lifetime_attribute(fn.return_lifetime, fn_attrs.lifetime, fn_attr_start_tok, "a function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        LifetimeAnnotation return_lifetime_for_hoist = fn.return_lifetime;
        if (auto _rv = hoist_type_lifetime_annotation(fn.return_type, return_lifetime_for_hoist, fn_attr_start_tok, "a function declarator"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        fn.return_lifetime = std::move(return_lifetime_for_hoist);

        // ch05 §5.11: a function with at least one concept-constrained
        // parameter is a *generic template* -- checked once, abstractly,
        // against each constrained parameter's own witness class; never
        // emitted to codegen directly (see Function::is_generic_template's
        // own comment). Computed here (rather than by scanning
        // program.functions later) since this is the one place every
        // function's parameter list is fully parsed, regardless of which
        // top-level path reached it.
        for (const Param& fn_param : fn.params) {
            if (!fn_param.generic_concept.empty()) {
                fn.is_generic_template = true;
                break;
            }
        }

        if (fn.has_varargs && !fn.is_extern_c) {
            const Token& tok = peek();
            {
                std::string _msg_7385{"variadic parameters ('...') are only supported in an 'extern \"C\"' "};
                _msg_7385 += "declaration (ch02 §2.1)";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_7385));
            }
        }

        if (match(TokenKind::Semicolon)) {
            return fn;
        }

        // [dcl.fct.def.delete]/1: a free function may be defined as
        // deleted. `= default` is grammatically available here too and is
        // rejected further on by validate_defaulted_special_member, which
        // already owns the "which functions may be defaulted" question --
        // parsing it here and rejecting it there keeps that one question
        // in one place instead of splitting it across the grammar.
        if (match(TokenKind::Assign)) {
            if (auto _rv = parse_deleted_defaulted_or_pure_suffix(fn, /*allow_default=*/false, /*allow_pure=*/false, "a function declaration");
                !_rv.has_value()) {
                return std::unexpected(std::move(_rv).error());
            }
            return fn;
        }

        if (fn.has_varargs) {
            const Token& tok = peek();
            {
                std::string _msg_7396{"variadic parameters ('...') are only supported for a bodyless "};
                _msg_7396 += "'extern \"C\"' declaration, not a definition (ch02 §2.1)";
                return std::unexpected(ParseError(tok.line, tok.column,
                              _msg_7396));
            }
        }
        auto _tmp_result = parse_block();
        if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
        fn.body = std::move(_tmp_result).value();
        return fn;
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_block() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        local_type_name_scopes_.emplace_back();
        auto block = std::make_unique<Stmt>();
        block->kind = StmtKind::Block;
        block->loc = loc;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            auto stmt_result = parse_statement();
            if (!stmt_result.has_value()) return std::unexpected(std::move(stmt_result).error());
            StmtPtr __stmt_result_value = std::move(stmt_result).value();
            block->statements.push_back(std::move(__stmt_result_value));
        }
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        local_type_name_scopes_.pop_back();
        return std::move(block);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_local_type_definition_statement() {
        SourceLocation loc = current_loc();
        if (current_program_ == nullptr) {
            return std::unexpected(ParseError(loc.line, loc.column, "internal parser error: local type definition without active program"));
        }
        if (check(TokenKind::KwStruct)) {
            std::vector<GenericTypeParam> no_template_params{};
            std::vector<AlignmentSpecifier> no_leading_alignments{};
            std::optional<std::string> no_forced_name{};
            // current_program_ is a raw Program* known non-null here (the
            // `current_program_ == nullptr` check just above); self-hosting
            // still requires an explicit `[[scpp::unsafe]] { }` to
            // dereference any raw pointer (ch01 §1.3/ch02).
            [[scpp::unsafe]] {
                auto struct_result = parse_struct_def(*current_program_, /*is_exported=*/false, std::move(no_template_params), std::move(no_leading_alignments), std::move(no_forced_name), true);
                if (!struct_result.has_value()) return std::unexpected(std::move(struct_result).error());
                StructDef __struct_result_value = std::move(struct_result).value();
                current_program_->structs.push_back(std::move(__struct_result_value));
            }
        } else {
            std::vector<GenericTypeParam> no_template_params{};
            std::vector<AlignmentSpecifier> no_leading_alignments{};
            std::optional<std::string> no_forced_name{};
            [[scpp::unsafe]] {
                if (auto _rv = parse_class_def(*current_program_, /*is_exported=*/false, std::move(no_template_params), std::move(no_leading_alignments), std::move(no_forced_name), true); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            }
        }
        return make_block_stmt(loc);
    }

    [[nodiscard]] std::expected<void, ParseError> reject_nested_fallthrough(const Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::Fallthrough:
                return std::unexpected(ParseError(stmt.loc.line, stmt.loc.column,
                                 "'[[fallthrough]];' is only valid as the final top-level statement of a switch case"));
            case StmtKind::If:
                if (stmt.then_branch != nullptr) { if (auto _rv = reject_nested_fallthrough(*stmt.then_branch); !_rv.has_value()) return std::unexpected(std::move(_rv).error()); }
                if (stmt.else_branch != nullptr) { if (auto _rv = reject_nested_fallthrough(*stmt.else_branch); !_rv.has_value()) return std::unexpected(std::move(_rv).error()); }
                return {};
            case StmtKind::While:
                if (stmt.then_branch != nullptr) { if (auto _rv = reject_nested_fallthrough(*stmt.then_branch); !_rv.has_value()) return std::unexpected(std::move(_rv).error()); }
                return {};
            case StmtKind::Switch:
                return {};
            case StmtKind::Block:
                for (const StmtPtr& child : stmt.statements) { if (auto _rv = reject_nested_fallthrough(*child); !_rv.has_value()) return std::unexpected(std::move(_rv).error()); }
                return {};
            case StmtKind::VarDecl:
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::ExprStmt:
                return {};
        }
        return {};
    }

    // Does `stmt` -- a statement sitting at the tail of a switch case's
    // braced body -- explicitly terminate that case? `break;`/`return
    // ...;`/`continue;` do, and a further nested block does iff its own
    // last statement does, so `case X: { { return v; } }` composes
    // naturally. An empty `{ }` terminates nothing, so it is not a
    // terminator (`case X: { }` stays an error).
    //
    // `[[fallthrough]];` is deliberately absent here: it is only ever
    // valid as a case's own *final top-level* statement, and
    // reject_nested_fallthrough above already rejects it anywhere else
    // with a diagnostic that says exactly that. Accepting it from inside
    // a block would be wrong anyway -- `[[fallthrough]];` describes
    // control leaving this case for the next one, which is a property of
    // the case, not of some inner block that happens to end with it.
    // Spelling that out here (rather than leaning on the fact that
    // reject_nested_fallthrough runs first and would have already
    // errored) keeps this predicate correct on its own terms.
    [[nodiscard]] bool block_body_terminates_switch_case(const Stmt& stmt) {
        if (stmt.kind == StmtKind::Break || stmt.kind == StmtKind::Return || stmt.kind == StmtKind::Continue) {
            return true;
        }
        if (stmt.kind == StmtKind::Block && !stmt.statements.empty()) {
            return block_body_terminates_switch_case(*stmt.statements.back());
        }
        return false;
    }

    // The rule being enforced is "no implicit fallthrough": every non-empty
    // case must end in a statement that explicitly says where control goes
    // next. A braced case body whose own last statement is such a statement
    // satisfies that rule exactly as well as a bare one does, so it is
    // accepted -- braces additionally give each case its own scope, so
    // same-named locals in sibling cases no longer collide.
    //
    // This stays a purely *syntactic* tail check, though: a trailing
    // `if`/`else` whose branches all return is still rejected, because
    // proving that needs the flow analysis this check deliberately does
    // not do.
    [[nodiscard]] bool is_explicit_switch_case_terminator(const Stmt& stmt) {
        return stmt.kind == StmtKind::Fallthrough || block_body_terminates_switch_case(stmt);
    }

    [[nodiscard]] std::expected<void, ParseError> validate_switch_fallthrough(const Stmt& stmt) {
        for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
            const SwitchCase& switch_case = stmt.switch_cases[i];
            for (std::size_t j = 0; j < switch_case.statements.size(); j++) {
                const Stmt& child = *switch_case.statements[j];
                if (child.kind == StmtKind::Fallthrough) {
                    if (j + 1 != switch_case.statements.size()) {
                        return std::unexpected(ParseError(child.loc.line, child.loc.column,
                                         "'[[fallthrough]];' must be the last statement in its switch case"));
                    }
                    if (i + 1 == stmt.switch_cases.size()) {
                        return std::unexpected(ParseError(child.loc.line, child.loc.column,
                                         "'[[fallthrough]];' requires a following case or default label"));
                    }
                } else {
                    if (auto _rv = reject_nested_fallthrough(child); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
                }
            }
            if (switch_case.statements.empty()) {
                if (i + 1 == stmt.switch_cases.size()) {
                    return std::unexpected(ParseError(switch_case.loc.line, switch_case.loc.column,
                                     "an empty switch case must be immediately followed by another case or default label"));
                }
                continue;
            }
            const Stmt& tail = *switch_case.statements.back();
            if (!is_explicit_switch_case_terminator(tail)) {
                {
                    std::string _msg_7501{"a non-empty switch case must end with 'break;', 'return ...;', 'continue;', or "};
                    _msg_7501 += "'[[fallthrough]];'";
                    return std::unexpected(ParseError(tail.loc.line, tail.loc.column,
                                 _msg_7501));
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_statement() {
        // Depth-checking wrapper; parse_statement_inner is the parser
        // proper. Written as a wrapper rather than as increments spread
        // through the body because the body has dozens of return
        // statements and every one of them would have to decrement.
        nesting_depth_++;
        if (nesting_depth_ > kMaxNestingDepth) {
            nesting_depth_--;
            return std::unexpected(nesting_too_deep_error("statement"));
        }
        auto result = parse_statement_inner();
        nesting_depth_--;
        return result;
    }

    [[nodiscard]] ParseError nesting_too_deep_error(const std::string& what) {
        std::string message{};
        message += "nesting is too deep: this ";
        message += what;
        message += " is more than ";
        message += std::to_string(static_cast<std::int64_t>(kMaxNestingDepth));
        message += " levels deep, which exceeds the maximum nesting depth the compiler supports";
        return ParseError(peek().line, peek().column, message);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_statement_inner() {
        // ch00 §2/ch01 §1.3: `[[scpp::unsafe]] { ... }` -- attribute-
        // driven now, not a keyword. Parses (and discards) any leading
        // attribute-specifier-seq first (real C++ grammar already
        // allows one on any statement), then parses whatever statement
        // follows normally; only when that statement turns out to be a
        // Block *and* `scpp::unsafe` was among the recognized attributes
        // does it get marked `is_unsafe` -- an attribute on any other
        // statement shape (e.g. a real C++ one like `[[likely]] if
        // (...) {}`) is accepted (mirrors a real compiler silently
        // accepting an attribute it doesn't act on) but has no scpp
        // effect at all, since only a compound-statement is a
        // recognized placement for `scpp::unsafe` (ch01 §1.3).
        if (check(TokenKind::LBracket) && peek_at(1).kind == TokenKind::LBracket) {
            const Token& attr_start_tok = peek();
            auto attrs_result = parse_attribute_specifier_seq();
            if (!attrs_result.has_value()) return std::unexpected(std::move(attrs_result).error());
            ParsedAttributes attrs = std::move(attrs_result).value();
            if (auto _rv = reject_packed_attribute(attrs, attr_start_tok, "a statement"); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
            if (attrs.has_fallthrough) {
                if (!check(TokenKind::Semicolon)) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column,
                                     "'[[fallthrough]]' only applies to a standalone ';' statement"));
                }
                if (switch_depth_ == 0) {
                    return std::unexpected(ParseError(attr_start_tok.line, attr_start_tok.column,
                                     "'[[fallthrough]];' is only valid inside a switch statement"));
                }
                if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return make_fallthrough_stmt(make_source_location(attr_start_tok.line, attr_start_tok.column, source_path_));
            }
            auto stmt_result = parse_statement();
            if (!stmt_result.has_value()) return std::unexpected(std::move(stmt_result).error());
            StmtPtr stmt = std::move(stmt_result).value();
            if (attrs.has("unsafe") && stmt->kind == StmtKind::Block) stmt->is_unsafe = true;
            return std::move(stmt);
        }
        if (check(TokenKind::LBrace)) return parse_block();
        if (check(TokenKind::KwStruct) || check(TokenKind::KwClass)) return parse_local_type_definition_statement();
        if (check(TokenKind::KwStatic)) return parse_var_decl();
        if (looks_like_type_start()) return parse_var_decl();
        if (check(TokenKind::KwReturn)) return parse_return();
        if (check(TokenKind::KwIf)) return parse_if();
        if (check(TokenKind::KwWhile)) return parse_while();
        if (check(TokenKind::KwSwitch)) return parse_switch();
        if (check(TokenKind::KwFor)) return parse_for();
        if (check(TokenKind::KwBreak)) return parse_break();
        if (check(TokenKind::KwContinue)) return parse_continue();
        return parse_expr_stmt();
    }

    // Whether the declaration starting here is spelled with `auto`
    // rather than a written type -- `auto`, or `const auto`, in either
    // case possibly followed by `&`.
    [[nodiscard]] bool at_auto_declaration_start() const {
        return check(TokenKind::KwAuto) || (check(TokenKind::KwConst) && peek_at(1).kind == TokenKind::KwAuto);
    }

    // The `[const] auto[&]` prefix of a declaration, in the one place
    // every declaration form asks about it. A range-for loop variable
    // (`for (auto& value : values)`, spec §10(7)-(8)) and an ordinary
    // variable declaration (`auto& v = ref_helper(x);`) are the same
    // grammar and must not be able to disagree: they did, because this
    // was written twice. parse_for_range_decl accepted `const` and `&`;
    // parse_var_decl_impl consumed `auto` and demanded a name straight
    // afterwards, so a reference binding to a deduced type -- the one
    // spelling for "borrow whatever this returns" -- was a *parse*
    // error, `expected variable name but found '&'`, and `const auto`
    // was `expected a type name`.
    //
    // `Reference` wrapping the `auto` sentinel is what the rest of the
    // pipeline already expects: monomorphize resolves the pointee from
    // the initializer for any VarDecl of this shape (its
    // `stmt.type.pointee->name == "auto"` case), which is how the
    // range-for spelling has always worked. Nothing downstream is
    // range-for-specific, so nothing downstream needed changing.
    //
    // The pointee is deliberately left unqualified even for `const
    // auto&`: constness rides on `is_mutable_ref`, and monomorphize
    // overwrites the placeholder pointee wholesale with the inferred
    // type, so qualifying it here would be discarded rather than
    // honoured -- exactly as the range-for path has always done it.
    [[nodiscard]] std::expected<Type, ParseError> parse_auto_declared_type(bool& out_is_const) {
        bool has_const = match(TokenKind::KwConst);
        if (auto _r = expect(TokenKind::KwAuto, "'auto'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        // East spelling: `auto const c` / `auto const& r`, the same
        // decl-specifier-seq written in the other order ([dcl.type.cv]
        // imposes no ordering). Without this the caller went on to
        // demand a variable name and reported `expected variable name
        // but found 'const'`.
        if (check(TokenKind::KwConst) && peek_at(1).kind != TokenKind::KwAuto) {
            advance();
            has_const = true;
        }
        Type declared = named_type("auto");
        if (check(TokenKind::AmpAmp)) {
            const Token& tok = peek();
            {
                std::string _msg_auto_rvalue{"'&&' (rvalue reference) is only supported for a function/method/"};
                _msg_auto_rvalue += "constructor parameter's declared type in this version (ch03) -- not ";
                _msg_auto_rvalue += "a variable, field, return type, or nested type argument, so 'auto&&' ";
                _msg_auto_rvalue += "is not a declaration form either";
                return std::unexpected(ParseError(tok.line, tok.column, _msg_auto_rvalue));
            }
        }
        if (check(TokenKind::Star)) {
            const Token& tok = peek();
            {
                std::string _msg_auto_pointer{"'auto*' is not a declaration form -- a plain 'auto' already deduces "};
                _msg_auto_pointer += "a pointer type from a pointer initializer, so write 'auto p = &x;'";
                return std::unexpected(ParseError(tok.line, tok.column, _msg_auto_pointer));
            }
        }
        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(declared));
            Type reference_type{};
            reference_type.kind = TypeKind::Reference;
            reference_type.pointee = std::move(pointee);
            reference_type.is_mutable_ref = !has_const;
            return reference_type;
        }
        out_is_const = has_const;
        return declared;
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_var_decl_impl(std::vector<AlignmentSpecifier> leading_alignments, bool require_semicolon,
                                bool require_explicit_initializer, bool qualify_variable_name) {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        stmt->loc = loc;
        stmt->alignment_specs = std::move(leading_alignments);
        if (!qualify_variable_name && match(TokenKind::KwStatic)) stmt->is_static_local = true;
        stmt->is_constexpr = match(TokenKind::KwConstexpr);
        if (at_auto_declaration_start()) {
            // ch05 §5.12: `auto name = expr;` infers the local's type
            // from its initializer -- the only way to name a closure's
            // own compiler-synthesized, otherwise unspellable class
            // type. `Type{Named, "auto"}` is a safe, collision-free
            // sentinel ("auto" is a reserved keyword -- the lexer never
            // produces it as an ordinary Identifier, so no real type can
            // ever be named this): resolved in place by movecheck's
            // Monomorphizer pass (monomorphize_generics), in the same
            // pre-check_moves phase that resolves a Lambda literal's own
            // synthesized class -- see its VarDecl case. Never reaches
            // check_moves/codegen unresolved. `const auto` and `auto&`
            // are the same declaration, parsed by the same helper the
            // range-for loop variable uses.
            auto auto_type_result = parse_auto_declared_type(stmt->is_const);
            if (!auto_type_result.has_value()) return std::unexpected(std::move(auto_type_result).error());
            stmt->type = std::move(auto_type_result).value();
            auto var_name_result = expect(TokenKind::Identifier, "variable name");
            if (!var_name_result.has_value()) return std::unexpected(std::move(var_name_result).error());
            stmt->var_name = std::string(var_name_result.value().text.data(), var_name_result.value().text.size());
            if (qualify_variable_name) stmt->var_name = qualify_name(stmt->var_name);
            const Token& tok = peek();
            if (!match(TokenKind::Assign)) {
                {
                    std::string _msg_7589{"'auto' requires an initializer ('auto name = expr;') -- there is no other "};
                    _msg_7589 += "way to know what concrete type to infer";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                  _msg_7589));
                }
            }
            auto init_result = parse_expr();
            if (!init_result.has_value()) return std::unexpected(std::move(init_result).error());
            stmt->init = std::move(init_result).value();
            if (require_semicolon) { if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error()); }
            return std::move(stmt);
        }
        auto base_result = parse_type(/*allow_rvalue_ref=*/false, &stmt->is_const);
        if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
        Type base = std::move(base_result).value();
        if (check(TokenKind::KwAlignas)) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'alignas' must appear before the declared type in a variable declaration (spec §9.3)"));
        }
        if (starts_function_pointer_declarator()) {
            auto type_result = parse_function_pointer_declarator(std::move(base), stmt->var_name);
            if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
            stmt->type = std::move(type_result).value();
        } else {
            auto var_name_result = expect(TokenKind::Identifier, "variable name");
            if (!var_name_result.has_value()) return std::unexpected(std::move(var_name_result).error());
            stmt->var_name = std::string(var_name_result.value().text.data(), var_name_result.value().text.size());
            auto type_result = parse_array_suffix(base);
            if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
            stmt->type = std::move(type_result).value();
        }
        if (qualify_variable_name) stmt->var_name = qualify_name(stmt->var_name);
        if (match(TokenKind::Assign)) {
            auto init_result = parse_brace_initializer_element();
            if (!init_result.has_value()) return std::unexpected(std::move(init_result).error());
            stmt->init = std::move(init_result).value();
        } else if (check(TokenKind::LBrace)) {
            // `ClassName name{args};` (ch04 §4.2 / spec §6.1): direct-
            // initialization via an explicit constructor call -- the
            // concrete way a `class`-typed local is constructed in this
            // version (there is no `=`-initializer form for a class type
            // yet, only this or a bare, zero-initialized declaration
            // calling no constructor at all, e.g. `ClassName name;`).
            // Movecheck/codegen resolve the callee by recomputing
            // `ClassName_new` from `stmt->type`, not from anything
            // recorded here.
            stmt->has_ctor_args = true;
            auto ctor_args_result = parse_brace_initializer_args();
            if (!ctor_args_result.has_value()) return std::unexpected(std::move(ctor_args_result).error());
            stmt->ctor_args = std::move(ctor_args_result).value();
        } else if (match(TokenKind::LParen)) {
            const Token& tok = peek();
            {
                std::string _msg_7640{"parenthesized direct-initialization is not allowed for object declarations; "};
                _msg_7640 += "use brace-init instead ('";
                _msg_7640 += stmt->type.name;
                _msg_7640 += " ";
                _msg_7640 += stmt->var_name;
                _msg_7640 += "{...};')";
                return std::unexpected(ParseError(tok.line, tok.column,
                             _msg_7640));
            }
        }
        // A `const`-qualified local (Stmt::is_const, set above by
        // parse_type via its out_bare_const out-parameter) must be
        // initialized right here -- there is no other opportunity to
        // ever give it a value, unlike an ordinary mutable local, which
        // may be declared bare and assigned later. Matches real C++'s
        // own "default initialization of const variable" rejection.
        if (require_explicit_initializer && !stmt->is_static_local && stmt->type.kind != TypeKind::Array && stmt->init == nullptr &&
            !stmt->has_ctor_args) {
            {
                std::string _msg_7653{"a non-array local variable declaration must include an explicit initializer "};
                _msg_7653 += "(write '";
                _msg_7653 += stmt->type.name;
                _msg_7653 += " ";
                _msg_7653 += stmt->var_name;
                _msg_7653 += "{};', '";
                _msg_7653 += stmt->type.name;
                _msg_7653 += " ";
                _msg_7653 += stmt->var_name;
                _msg_7653 += "{...};', or '";
                _msg_7653 += stmt->type.name;
                _msg_7653 += " ";
                _msg_7653 += stmt->var_name;
                _msg_7653 += " = ...;')";
                return std::unexpected(ParseError(loc.line, loc.column,
                             _msg_7653));
            }
        }
        // [dcl.constexpr]/1: "applied to the definition of a variable
        // [constexpr] implies const". Recorded on the type, like the
        // written `const` -- otherwise `constexpr char w[6]{"hello"};
        // char* p = w;` decayed to a *mutable* `char*` and wrote through
        // a constexpr object, while the identically-meaning `const char
        // w[6]` did not.
        if (stmt->is_constexpr) stmt->type.is_const_qualified = true;
        if (((stmt->is_const && !stmt->is_static_local) || stmt->is_constexpr) && stmt->init == nullptr && !stmt->has_ctor_args) {
            {
                std::string _msg_7660{"a constant variable must be initialized ('"};
                _msg_7660 += std::string(stmt->is_constexpr ? "constexpr " : "const ");
                _msg_7660 += stmt->type.name;
                _msg_7660 += " ";
                _msg_7660 += stmt->var_name;
                _msg_7660 += " = ...;') -- it can never be given a value afterward";
                return std::unexpected(ParseError(loc.line, loc.column,
                              _msg_7660));
            }
        }
        if (require_semicolon) {
            if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_var_decl(bool require_semicolon = true) {
        auto alignments_result = parse_alignment_specifier_seq();
        if (!alignments_result.has_value()) return std::unexpected(std::move(alignments_result).error());
        std::vector<AlignmentSpecifier> __alignments_value = std::move(alignments_result).value();
        return parse_var_decl_impl(std::move(__alignments_value), require_semicolon,
                                   /*require_explicit_initializer=*/true,
                                   /*qualify_variable_name=*/false);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_global_var_decl(std::vector<AlignmentSpecifier> leading_alignments) {
        auto stmt_result = parse_var_decl_impl(std::move(leading_alignments), /*require_semicolon=*/true,
                                          /*require_explicit_initializer=*/false,
                                          /*qualify_variable_name=*/true);
        if (!stmt_result.has_value()) return std::unexpected(std::move(stmt_result).error());
        StmtPtr stmt = std::move(stmt_result).value();
        if (stmt->type.kind == TypeKind::Reference && stmt->init == nullptr) {
            return std::unexpected(ParseError(stmt->loc.line, stmt->loc.column,
                             "a reference variable must be initialized at declaration"));
        }
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_for_range_decl() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        stmt->loc = loc;
        auto alignments_result = parse_alignment_specifier_seq();
        if (!alignments_result.has_value()) return std::unexpected(std::move(alignments_result).error());
        stmt->alignment_specs = std::move(alignments_result).value();

        if (at_auto_declaration_start()) {
            auto auto_type_result = parse_auto_declared_type(stmt->is_const);
            if (!auto_type_result.has_value()) return std::unexpected(std::move(auto_type_result).error());
            Type declared = std::move(auto_type_result).value();
            auto var_name_result = expect(TokenKind::Identifier, "variable name");
            if (!var_name_result.has_value()) return std::unexpected(std::move(var_name_result).error());
            stmt->var_name = std::string(var_name_result.value().text.data(), var_name_result.value().text.size());
            stmt->type = std::move(declared);
            return std::move(stmt);
        }

        auto base_result = parse_type(/*allow_rvalue_ref=*/false, &stmt->is_const);
        if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
        Type base = std::move(base_result).value();
        if (check(TokenKind::KwAlignas)) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column,
                             "'alignas' must appear before the declared type in a variable declaration (spec §9.3)"));
        }
        if (starts_function_pointer_declarator()) {
            auto type_result = parse_function_pointer_declarator(std::move(base), stmt->var_name);
            if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
            stmt->type = std::move(type_result).value();
        } else {
            auto var_name_result = expect(TokenKind::Identifier, "variable name");
            if (!var_name_result.has_value()) return std::unexpected(std::move(var_name_result).error());
            stmt->var_name = std::string(var_name_result.value().text.data(), var_name_result.value().text.size());
            stmt->type = std::move(base);
        }
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_return() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwReturn, "'return'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Return;
        stmt->loc = loc;
        if (!check(TokenKind::Semicolon)) {
            // Bare `return {};` (no leading type/identifier at all) --
            // distinct from the `Identifier{args}` case just below, which
            // still goes through parse_expr() first since it has a real
            // leading expression to parse. Here there is nothing for
            // parse_expr()/parse_primary() to parse at all (a lone `{` is
            // not a valid expression on its own), so this is special-cased
            // directly on the raw tokens before parse_expr() ever runs.
            // The target type is left for monomorphization to fill in
            // (see ExprKind::ValueInit, ast.cppm) from the enclosing
            // function's own return type, which is unambiguous here.
            if (check(TokenKind::LBrace) && pos_ + 1 < tokens_.size() && tokens_[pos_ + 1].kind == TokenKind::RBrace) {
                advance();
                advance();
                auto value_init = std::make_unique<Expr>();
                value_init->kind = ExprKind::ValueInit;
                value_init->loc = loc;
                stmt->expr = std::move(value_init);
                if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return std::move(stmt);
            }
            auto expr_result = parse_brace_initializer_element();
            if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
            stmt->expr = std::move(expr_result).value();
            if (stmt->expr != nullptr && stmt->expr->kind == ExprKind::Identifier && check(TokenKind::LBrace)) {
                auto call = std::make_unique<Expr>();
                call->kind = ExprKind::Call;
                call->loc = stmt->expr->loc;
                call->name = stmt->expr->name;
                call->explicit_global_qualification = stmt->expr->explicit_global_qualification;
                std::vector<ExplicitTemplateArg>& explicit_template_args_ref = stmt->expr->explicit_template_args;
                call->explicit_template_args = std::move(explicit_template_args_ref);
                auto args_result = parse_brace_initializer_args();
                if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                call->args = std::move(args_result).value();
                stmt->expr = std::move(call);
            }
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_if() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwIf, "'if'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::If;
        stmt->loc = loc;
        StmtPtr init_stmt{};
        if (match(TokenKind::KwConsteval)) {
            stmt->if_mode = IfMode::ConstevalTrue;
            stmt->condition = make_bool_literal_expr(loc, true);
        } else if (match(TokenKind::Bang)) {
            if (auto _r = expect(TokenKind::KwConsteval, "'consteval'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            stmt->if_mode = IfMode::ConstevalFalse;
            stmt->condition = make_bool_literal_expr(loc, false);
        } else {
            if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            // ch05: an "if" statement's own init-statement (C++17) --
            // `if (auto x = f(); cond) ...` -- desugared at parse time
            // into `{ auto x = f(); if (cond) ... }` (see
            // desugar_if_with_init below) rather than threading a new
            // "this If also carries its own init-statement" concept
            // through every later pass (movecheck/monomorphize/codegen/
            // constexpr), exactly mirroring how a classic `for` loop's
            // own init-statement is already handled entirely at parse
            // time (see parse_for/desugar_classic_for just above/below).
            // Disambiguated the same way parse_for's own init-clause is:
            // a leading type-looking token (or `auto`) means this is a
            // declaration, consumed without its own trailing `;`
            // (require_semicolon=false) since that belongs to *this*
            // clause, not the declaration itself -- anything else is an
            // ordinary condition expression with no init-statement at
            // all, exactly like before.
            if (looks_like_type_start()) {
                auto init_stmt_result = parse_var_decl(/*require_semicolon=*/false);
                if (!init_stmt_result.has_value()) return std::unexpected(std::move(init_stmt_result).error());
                init_stmt = std::move(init_stmt_result).value();
                if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            auto condition_result = parse_expr();
            if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
            stmt->condition = std::move(condition_result).value();
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        }
        auto then_result = parse_statement();
        if (!then_result.has_value()) return std::unexpected(std::move(then_result).error());
        stmt->then_branch = std::move(then_result).value();
        if (match(TokenKind::KwElse)) {
            auto else_result = parse_statement();
            if (!else_result.has_value()) return std::unexpected(std::move(else_result).error());
            stmt->else_branch = std::move(else_result).value();
        }
        if (init_stmt != nullptr) return desugar_if_with_init(loc, std::move(init_stmt), std::move(stmt));
        return std::move(stmt);
    }

    // See parse_if's own comment: wraps an "if" statement that had its
    // own C++17 init-statement in a synthetic enclosing Block (mirrors
    // desugar_classic_for's identical outer_block/init_stmt pattern),
    // scoping the declaration to exactly this if/else-if/else chain and
    // nothing after it -- every later pass already knows how to handle
    // an ordinary Block+If pair, so this needs no further support
    // anywhere else in the compiler.
    [[nodiscard]] StmtPtr desugar_if_with_init(SourceLocation loc, StmtPtr init_stmt, StmtPtr if_stmt) {
        auto outer_block = make_block_stmt(loc);
        outer_block->statements.push_back(std::move(init_stmt));
        outer_block->statements.push_back(std::move(if_stmt));
        return outer_block;
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_while() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwWhile, "'while'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::While;
        stmt->loc = loc;
        auto condition_result = parse_expr();
        if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
        stmt->condition = std::move(condition_result).value();
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        loop_depth_++;
        auto then_result = parse_statement();
        loop_depth_--;
        if (!then_result.has_value()) return std::unexpected(std::move(then_result).error());
        stmt->then_branch = std::move(then_result).value();
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_switch() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwSwitch, "'switch'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Switch;
        stmt->loc = loc;
        auto condition_result = parse_expr();
        if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
        stmt->condition = std::move(condition_result).value();
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::LBrace, "'{'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        switch_depth_++;
        bool saw_default = false;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            SwitchCase switch_case{};
            switch_case.loc = current_loc();
            if (match(TokenKind::KwCase)) {
                auto _tmp_result = parse_expr();
                if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
                switch_case.value = std::move(_tmp_result).value();
                if (auto _r = expect(TokenKind::Colon, "':'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            } else if (match(TokenKind::KwDefault)) {
                if (saw_default) {
                    const Token& tok = peek();
                    return std::unexpected(ParseError(tok.line, tok.column, "a switch statement may contain at most one 'default' label"));
                }
                saw_default = true;
                if (auto _r = expect(TokenKind::Colon, "':'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            } else {
                const Token& tok = peek();
                return std::unexpected(ParseError(tok.line, tok.column, "expected 'case' or 'default' inside switch body"));
            }
            while (!check(TokenKind::RBrace) && !check(TokenKind::KwCase) && !check(TokenKind::KwDefault) &&
                   !check(TokenKind::EndOfFile)) {
                auto case_stmt_result = parse_statement();
                if (!case_stmt_result.has_value()) return std::unexpected(std::move(case_stmt_result).error());
                StmtPtr __case_stmt_result_value = std::move(case_stmt_result).value();
                switch_case.statements.push_back(std::move(__case_stmt_result_value));
            }
            stmt->switch_cases.push_back(std::move(switch_case));
        }
        switch_depth_--;
        if (auto _r = expect(TokenKind::RBrace, "'}'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _rv = validate_switch_fallthrough(*stmt); !_rv.has_value()) return std::unexpected(std::move(_rv).error());
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_for() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwFor, "'for'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        if (is_range_for_decl_start()) {
            std::size_t saved_pos = pos_;
            auto loop_var_result = parse_for_range_decl();
            bool has_loop_var_result = loop_var_result.has_value();
            StmtPtr loop_var{};
            if (has_loop_var_result) {
                loop_var = std::move(loop_var_result).value();
            }
            if (!has_loop_var_result) {
                pos_ = saved_pos;
            }
            if (loop_var != nullptr) {
                if (match(TokenKind::Colon)) {
                    auto range_expr_result = parse_expr();
                    if (!range_expr_result.has_value()) return std::unexpected(std::move(range_expr_result).error());
                    ExprPtr range_expr = std::move(range_expr_result).value();
                    if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    loop_depth_++;
                    auto body_result = parse_statement();
                    if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
                    StmtPtr body = std::move(body_result).value();
                    loop_depth_--;
                    return desugar_range_for(loc, std::move(loop_var), std::move(range_expr), std::move(body));
                }
                pos_ = saved_pos;
            }
        }

        StmtPtr init_stmt{};

        if (!check(TokenKind::Semicolon)) {
            if (looks_like_type_start()) {
                auto init_stmt_result = parse_var_decl(/*require_semicolon=*/false);
                if (!init_stmt_result.has_value()) return std::unexpected(std::move(init_stmt_result).error());
                init_stmt = std::move(init_stmt_result).value();
            } else {
                auto init_expr_result = parse_expr();
                if (!init_expr_result.has_value()) return std::unexpected(std::move(init_expr_result).error());
                ExprPtr __init_expr_value = std::move(init_expr_result).value();
                init_stmt = make_expr_stmt(current_loc(), std::move(__init_expr_value));
            }
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        ExprPtr condition{};

        if (check(TokenKind::Semicolon)) {
            condition = make_bool_literal_expr(loc, true);
        } else {
            auto condition_result = parse_expr();
            if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
            condition = std::move(condition_result).value();
        }
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        ExprPtr increment{};

        if (!check(TokenKind::RParen)) {
            auto increment_result = parse_expr();
            if (!increment_result.has_value()) return std::unexpected(std::move(increment_result).error());
            increment = std::move(increment_result).value();
        }
        if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        loop_depth_++;
        auto body_result = parse_statement();
        if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
        StmtPtr body = std::move(body_result).value();
        loop_depth_--;
        return desugar_classic_for(loc, std::move(init_stmt), std::move(condition), std::move(increment), std::move(body));
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_break() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwBreak, "'break'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (loop_depth_ == 0 && switch_depth_ == 0) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column, "'break' is only valid inside a loop or switch"));
        }
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Break;
        stmt->loc = loc;
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_continue() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::KwContinue, "'continue'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        if (loop_depth_ == 0) {
            const Token& tok = peek();
            return std::unexpected(ParseError(tok.line, tok.column, "'continue' is only valid inside a loop"));
        }
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Continue;
        stmt->loc = loc;
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return std::move(stmt);
    }

    [[nodiscard]] std::expected<StmtPtr, ParseError> parse_expr_stmt() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->loc = loc;
        auto expr_result = parse_expr();
        if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
        stmt->expr = std::move(expr_result).value();
        if (auto _r = expect(TokenKind::Semicolon, "';'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        return std::move(stmt);
    }

    // Precedence climbing, lowest to highest:
    // assignment -> conditional -> logic_or -> logic_and -> equality -> relational
    // -> additive -> multiplicative -> unary -> primary

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_expr() { return parse_assignment(); }

    std::optional<BinaryOp> parse_assignment_operator() {
        if (match(TokenKind::Assign)) { BinaryOp op = BinaryOp::Assign; return op; }
        if (match(TokenKind::PlusAssign)) { BinaryOp op = BinaryOp::AddAssign; return op; }
        if (match(TokenKind::MinusAssign)) { BinaryOp op = BinaryOp::SubAssign; return op; }
        if (match(TokenKind::StarAssign)) { BinaryOp op = BinaryOp::MulAssign; return op; }
        if (match(TokenKind::SlashAssign)) { BinaryOp op = BinaryOp::DivAssign; return op; }
        return std::optional<BinaryOp>{};
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_assignment() {
        nesting_depth_++;
        if (nesting_depth_ > kMaxNestingDepth) {
            nesting_depth_--;
            return std::unexpected(nesting_too_deep_error("expression"));
        }
        auto result = parse_assignment_inner();
        nesting_depth_--;
        return result;
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_assignment_inner() {
        auto lhs_result = parse_conditional();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        if (std::optional<BinaryOp> op = parse_assignment_operator(); op.has_value()) {
            auto rhs_result = parse_assignment();
            if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
            ExprPtr rhs = std::move(rhs_result).value();
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Binary;
            node->binary_op = *op;
            node->loc = lhs->loc;
            node->lhs = std::move(lhs);
            node->rhs = std::move(rhs);
            return std::move(node);
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_conditional() {
        auto condition_result = parse_logic_or();
        if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
        ExprPtr condition = std::move(condition_result).value();
        if (!match(TokenKind::Question)) return std::move(condition);
        auto then_expr_result = parse_expr();
        if (!then_expr_result.has_value()) return std::unexpected(std::move(then_expr_result).error());
        ExprPtr then_expr = std::move(then_expr_result).value();
        if (auto _r = expect(TokenKind::Colon, "':'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
        auto else_expr_result = parse_conditional();
        if (!else_expr_result.has_value()) return std::unexpected(std::move(else_expr_result).error());
        ExprPtr else_expr = std::move(else_expr_result).value();
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Conditional;
        node->loc = condition->loc;
        node->lhs = std::move(condition);
        node->rhs = std::move(then_expr);
        node->third = std::move(else_expr);
        return std::move(node);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_logic_or() {
        auto lhs_result = parse_logic_and();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        while (check(TokenKind::PipePipe)) {
            // Left-associative chains are built by this loop, not by
            // recursion, so each iteration deepens the tree by one level
            // without deepening the parse -- count it explicitly or a
            // long chain slips past the recursion guard and overflows a
            // later pass instead. See kMaxNestingDepth in scpp.ast.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            advance();
            auto rhs_result = parse_logic_and();
            if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
            ExprPtr __rhs_value = std::move(rhs_result).value();
            lhs = make_binary(BinaryOp::Or, std::move(lhs), std::move(__rhs_value));
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_logic_and() {
        auto lhs_result = parse_equality();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        while (check(TokenKind::AmpAmp)) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            advance();
            auto rhs_result = parse_equality();
            if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
            ExprPtr __rhs_value = std::move(rhs_result).value();
            lhs = make_binary(BinaryOp::And, std::move(lhs), std::move(__rhs_value));
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_equality() {
        auto lhs_result = parse_relational();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        for (;;) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            if (match(TokenKind::EqualEqual)) {
                auto rhs_result = parse_relational();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Eq, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::NotEqual)) {
                auto rhs_result = parse_relational();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Ne, std::move(lhs), std::move(__rhs_value));
            } else {
                break;
            }
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_relational() {
        auto lhs_result = parse_additive();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        for (;;) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            if (match(TokenKind::Less)) {
                auto rhs_result = parse_additive();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Lt, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::Greater)) {
                auto rhs_result = parse_additive();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Gt, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::LessEqual)) {
                auto rhs_result = parse_additive();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Le, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::GreaterEqual)) {
                auto rhs_result = parse_additive();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Ge, std::move(lhs), std::move(__rhs_value));
            } else {
                break;
            }
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_additive() {
        auto lhs_result = parse_multiplicative();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        for (;;) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            if (match(TokenKind::Plus)) {
                auto rhs_result = parse_multiplicative();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Add, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::Minus)) {
                auto rhs_result = parse_multiplicative();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Sub, std::move(lhs), std::move(__rhs_value));
            } else {
                break;
            }
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_multiplicative() {
        auto lhs_result = parse_unary();
        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
        ExprPtr lhs = std::move(lhs_result).value();
        int chain_depth = 0;
        for (;;) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            if (match(TokenKind::Star)) {
                auto rhs_result = parse_unary();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Mul, std::move(lhs), std::move(__rhs_value));
            } else if (match(TokenKind::Slash)) {
                auto rhs_result = parse_unary();
                if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                ExprPtr __rhs_value = std::move(rhs_result).value();
                lhs = make_binary(BinaryOp::Div, std::move(lhs), std::move(__rhs_value));
            } else {
                break;
            }
        }
        return std::move(lhs);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_unary() {
        nesting_depth_++;
        if (nesting_depth_ > kMaxNestingDepth) {
            nesting_depth_--;
            return std::unexpected(nesting_too_deep_error("expression"));
        }
        auto result = parse_unary_inner();
        nesting_depth_--;
        return result;
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_unary_inner() {
        SourceLocation loc = current_loc();
        if (match(TokenKind::KwAlignof)) {
            if (auto _r = expect(TokenKind::LParen, "'(' after 'alignof'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            if (!looks_like_type_start()) {
                const Token& tok = peek();
                {
                    std::string _msg_8212{"'alignof' requires a type-id operand; GNU-style 'alignof(expression)' is not "};
                    _msg_8212 += "supported in SCPP26 (spec §9.3)";
                    return std::unexpected(ParseError(tok.line, tok.column,
                                 _msg_8212));
                }
            }
            auto queried_type_result = parse_type();
            if (!queried_type_result.has_value()) return std::unexpected(std::move(queried_type_result).error());
            Type queried_type = std::move(queried_type_result).value();
            if (auto _r = expect(TokenKind::RParen, "')' after alignof type operand"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Alignof;
            node->loc = loc;
            node->type = std::move(queried_type);
            return std::move(node);
        }
        if (match(TokenKind::KwSizeof)) {
            if (auto _r = expect(TokenKind::LParen, "'(' after 'sizeof'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            std::size_t saved_pos = pos_;
            bool parsed_as_type = false;
            std::unique_ptr<Expr> type_node{};

            if (looks_like_type_start()) {
                auto target_type_result = parse_type();
                if (target_type_result.has_value()) {
                    Type target_type = std::move(target_type_result).value();
                    if (auto _r = expect(TokenKind::RParen, "')' after sizeof type operand"); _r.has_value()) {
                        type_node = std::make_unique<Expr>();
                        type_node->kind = ExprKind::Sizeof;
                        type_node->loc = loc;
                        type_node->type = std::move(target_type);
                        type_node->sizeof_operand_is_type = true;
                        parsed_as_type = true;
                    }
                }
            }
            if (parsed_as_type) return std::move(type_node);
            pos_ = saved_pos;
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Sizeof;
            node->loc = loc;
            auto lhs_result = parse_expr();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            if (auto _r = expect(TokenKind::RParen, "')' after sizeof expression operand"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            return std::move(node);
        }
        // ch06 §6: `(T)expr` -- the C-style cast spelling (real C++
        // accepts both this and `static_cast<T>(expr)`; scpp's own
        // scalar-conversion table doesn't prefer one over the other, so
        // both are supported). Ambiguous in general with a parenthesized
        // expression (`(x)`), exactly like real C++'s own classic
        // ambiguity -- resolved here by speculative parsing: only when
        // the token right after `(` already looks like the start of a
        // type (scpp's type-name set is closed and known ahead of time,
        // ch05/ch11's struct_names_) is a type even attempted; if
        // parsing one then fails, or `)` doesn't immediately follow it,
        // this backtracks to `pos_`'s saved value and falls through to
        // parse_postfix(parse_primary())'s ordinary parenthesized-
        // expression handling below, unaffected.
        if (check(TokenKind::LParen) && looks_like_type_start_at(1)) {
            std::size_t saved_pos = pos_;
            bool parsed_as_cast = false;
            std::unique_ptr<Expr> node{};

            advance(); // '('
            auto target_type_result = parse_type();
            if (target_type_result.has_value()) {
                Type target_type = std::move(target_type_result).value();
                if (check(TokenKind::RParen)) {
                    advance(); // ')'
                    auto lhs_result = parse_unary();
                    if (lhs_result.has_value()) {
                        node = std::make_unique<Expr>();
                        node->kind = ExprKind::Cast;
                        node->loc = loc;
                        node->type = std::move(target_type);
                        node->lhs = std::move(lhs_result).value();
                        parsed_as_cast = true;
                    }
                }
            }
            // Not a type after all (e.g. `(x + y)` where `x` happens to
            // look like a type-start token but isn't followed by a
            // well-formed type), or the cast's operand itself failed to
            // parse -- fall through to backtracking below, same as the
            // "no ')' immediately after" case.
            if (parsed_as_cast) return std::move(node);
            pos_ = saved_pos;
        }
        if (match(TokenKind::Minus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Neg;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::PlusPlus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::PreInc;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::MinusMinus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::PreDec;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::Bang)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Not;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::Star)) {
            // `*p` (dereference) -- unambiguous with binary `*`
            // (multiplication) since a prefix operator only ever
            // appears where a new operand is expected, never between
            // two already-parsed operands.
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Deref;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::KwDelete)) {
            if (match(TokenKind::LBracket)) {
                if (auto _r = expect(TokenKind::RBracket, "']' after 'delete['"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                {
                    std::string _msg_8357{"'delete[]' is not supported in this version yet; only scalar/object 'delete "};
                    _msg_8357 += "expr' is implemented";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                  _msg_8357));
                }
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Delete;
            node->loc = loc;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        if (match(TokenKind::Amp)) {
            // `&expr` (address-of, ch05 §5.7) -- unlike `*`, `Amp` never
            // doubles as a binary operator in scpp (there is no bitwise
            // `&`; `T&`/`const T&` reference syntax is only recognized by
            // parse_type, never reached from an expression context), so
            // this is unconditionally a prefix operator here, no
            // position-based disambiguation needed. `expr.lhs`'s shape
            // (must resolve to a place) is a semantic check, not a
            // grammar one -- deferred to movecheck's
            // resolve_borrow_source_root, same division of labor as
            // Deref's operand above.
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::AddressOf;
            auto lhs_result = parse_unary();
            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
            node->lhs = std::move(lhs_result).value();
            return std::move(node);
        }
        auto primary_result = parse_primary();
        if (!primary_result.has_value()) return std::unexpected(std::move(primary_result).error());
        ExprPtr __primary_result_value = std::move(primary_result).value();
        return parse_postfix(std::move(__primary_result_value));
    }

    [[nodiscard]] std::expected<std::string, ParseError> parse_member_access_name() {
        if (check(TokenKind::Identifier) && std::string(peek().text.data(), peek().text.size()) == "operator") {
            advance(); // 'operator'
            if (match(TokenKind::Star)) { std::string name_result{"operator_deref"}; return name_result; }
            if (match(TokenKind::Arrow)) { std::string name_result{"operator_arrow"}; return name_result; }
            if (match(TokenKind::Assign)) { std::string name_result{"operator_assign"}; return name_result; }
            return std::unexpected(ParseError(current_loc().line, current_loc().column,
                             "expected '*', '->', or '=' after 'operator' in member access"));
        }
        auto tok_result = expect(TokenKind::Identifier, "field or method name");
        if (!tok_result.has_value()) return std::unexpected(std::move(tok_result).error());
        return std::string(tok_result.value().text.data(), tok_result.value().text.size());
    }

    // Applies trailing `.name` (Member, or a method call -- ch05 §5.9 --
    // if `(` follows), `->name` (resolved later via raw-pointer member
    // access or an explicit operator-> chain; `this->x` stays a special
    // case because `this` is modeled as a reference pseudo-parameter
    // rather than a real pointer), and `[index]` (Subscript) operators,
    // e.g. `p.x`, `arr[i]`, `p.inner.x`, `arr[i].x`, `p->x`,
    // `obj.method(args)`.
    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_postfix(ExprPtr expr) {
        int chain_depth = 0;
        for (;;) {
            // One tree level per iteration; see parse_logic_or's note.
            chain_depth++;
            if (nesting_depth_ + chain_depth > kMaxNestingDepth) {
                return std::unexpected(nesting_too_deep_error("expression"));
            }
            if (match(TokenKind::Dot)) {
                if (match(TokenKind::Tilde)) {
                    auto destroyed_type_result = parse_type();
                    if (!destroyed_type_result.has_value()) return std::unexpected(std::move(destroyed_type_result).error());
                    Type destroyed_type = std::move(destroyed_type_result).value();
                    if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    auto node = std::make_unique<Expr>();
                    node->kind = ExprKind::Destroy;
                    node->loc = expr->loc;
                    node->lhs = std::move(expr);
                    node->type = std::move(destroyed_type);
                    expr = std::move(node);
                    continue;
                }
                auto name_result = parse_member_access_name();
                if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
                std::string name = std::move(name_result).value();
                auto expr_result = parse_member_or_method_call(std::move(expr), name);
                if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
                expr = std::move(expr_result).value();
            } else if (match(TokenKind::Arrow)) {
                if (match(TokenKind::Tilde)) {
                    auto destroyed_type_result = parse_type();
                    if (!destroyed_type_result.has_value()) return std::unexpected(std::move(destroyed_type_result).error());
                    Type destroyed_type = std::move(destroyed_type_result).value();
                    if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    auto node = std::make_unique<Expr>();
                    node->kind = ExprKind::Destroy;
                    node->loc = expr->loc;
                    node->lhs = std::move(expr);
                    node->type = std::move(destroyed_type);
                    node->destroy_through_pointer = true;
                    expr = std::move(node);
                    continue;
                }
                auto name_result = parse_member_access_name();
                if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
                std::string name = std::move(name_result).value();
                // `this->x` (ch05 §5.9): `this` is represented as an
                // ordinary Reference-typed pseudo-parameter (see parser's
                // make_this_param), which already auto-dereferences on
                // every use (codegen_lvalue's Identifier case) exactly
                // like `a.x` already does for any other reference-typed
                // local `a` -- so unlike `p->x` for a real pointer/
                // unique_ptr `p` below, there is no separate pointee to
                // Deref through first.
                if (expr->kind == ExprKind::Identifier && expr->name == "this") {
                    auto expr_result = parse_member_or_method_call(std::move(expr), name);
                    if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
                    expr = std::move(expr_result).value();
                    continue;
                }
                auto expr_result = parse_member_or_method_call(std::move(expr), name, /*through_arrow=*/true);
                if (!expr_result.has_value()) return std::unexpected(std::move(expr_result).error());
                expr = std::move(expr_result).value();
            } else if (match(TokenKind::LBracket)) {
                auto index_result = parse_expr();
                if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                ExprPtr index = std::move(index_result).value();
                if (auto _r = expect(TokenKind::RBracket, "']'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Subscript;
                node->loc = expr->loc;
                node->lhs = std::move(expr);
                node->rhs = std::move(index);
                expr = std::move(node);
            } else if (match(TokenKind::Ellipsis)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::PackExpansion;
                node->loc = expr->loc;
                node->lhs = std::move(expr);
                expr = std::move(node);
            } else if (match(TokenKind::PlusPlus)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Unary;
                node->loc = expr->loc;
                node->unary_op = UnaryOp::PostInc;
                node->lhs = std::move(expr);
                expr = std::move(node);
            } else if (match(TokenKind::MinusMinus)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Unary;
                node->loc = expr->loc;
                node->unary_op = UnaryOp::PostDec;
                node->lhs = std::move(expr);
                expr = std::move(node);
            } else if (check(TokenKind::LParen)) {
                advance(); // '('
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->loc = expr->loc;
                if (expr->kind == ExprKind::Identifier) {
                    node->name = expr->name;
                    node->explicit_global_qualification = expr->explicit_global_qualification;
                    std::vector<ExplicitTemplateArg>& expr_explicit_template_args_ref = expr->explicit_template_args;
                    node->explicit_template_args = std::move(expr_explicit_template_args_ref);
                    std::size_t last_separator = node->name.rfind("::");
                    if (last_separator != static_cast<std::size_t>(-1)) {
                        std::string owner_name = node->name.substr(static_cast<std::size_t>(0), last_separator);
                        std::string member_name = node->name.substr(last_separator + 2);
                        if (owner_name.find('<') == static_cast<std::size_t>(-1)) {
                            if (std::optional<std::string> resolved_owner =
                                    resolve_static_member_owner_name(owner_name, node->explicit_global_qualification);
                                resolved_owner.has_value()) {
                                node->name = *resolved_owner;
                                node->name += "_";
                                node->name += member_name;
                                node->explicit_global_qualification = false;
                            }
                        }
                    }
                } else if (expr->kind == ExprKind::Lambda) {
                    node->name = "call";
                    node->lhs = std::move(expr);
                } else {
                    node->lhs = std::move(expr);
                }
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        auto arg_result = parse_brace_initializer_element();
                        if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                        ExprPtr __arg_result_value = std::move(arg_result).value();
                        node->args.push_back(std::move(__arg_result_value));
                        if (!(match(TokenKind::Comma))) break;
                    }
                }
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                expr = std::move(node);
            } else if (check(TokenKind::LBrace) && expr->kind == ExprKind::Identifier) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->loc = expr->loc;
                node->name = expr->name;
                node->explicit_global_qualification = expr->explicit_global_qualification;
                std::vector<ExplicitTemplateArg>& expr_explicit_template_args_ref2 = expr->explicit_template_args;
                node->explicit_template_args = std::move(expr_explicit_template_args_ref2);
                auto args_result = parse_brace_initializer_args();
                if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
                node->args = std::move(args_result).value();
                expr = std::move(node);
            } else {
                break;
            }
        }
        return std::move(expr);
    }

    // Shared by parse_postfix's `.name`/(this-adjusted) `->name` cases:
    // `name(args)` is a method call (ch05 §5.9) -- `base` (the receiver)
    // is stored in the resulting Call's `lhs` (nullptr for an ordinary
    // free-function call, see ast.cppm's Expr), resolved to a concrete
    // synthesized function symbol only once `base`'s static type is known
    // (movecheck/codegen, not the parser). Otherwise it's a plain field
    // access, unchanged from before method calls existed.
    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_member_or_method_call(ExprPtr base, const std::string& name, bool through_arrow = false) {
        SourceLocation loc = base->loc;
        if (match(TokenKind::LParen)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Call;
            node->loc = loc;
            node->name = name;
            node->lhs = std::move(base);
            node->through_arrow = through_arrow;
            if (!check(TokenKind::RParen)) {
                while (true) {
                    auto arg_result = parse_brace_initializer_element();
                    if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                    ExprPtr __arg_result_value = std::move(arg_result).value();
                    node->args.push_back(std::move(__arg_result_value));
                    if (!(match(TokenKind::Comma))) break;
                }
            }
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            return std::move(node);
        }
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Member;
        node->loc = loc;
        node->name = name;
        node->lhs = std::move(base);
        node->through_arrow = through_arrow;
        return std::move(node);
    }

    // ch05 §5.12: parses `[capture-list](params) [mutable] [-> Type] {
    // body }` -- reuses real C++ lambda syntax verbatim. Produces a
    // *raw* (unresolved) Lambda Expr: a capture's concrete type isn't
    // known yet (the parser has no type inference for arbitrary
    // enclosing-scope locals -- that's movecheck's job), and a blanket
    // `[=]`/`[&]` capture mode's own implicit captures aren't resolved
    // yet either (needs free-variable analysis over the body, likewise
    // deferred). See movecheck's closure-resolution pass (which runs in
    // the same pre-check_moves phase as concept monomorphization, for
    // the same reason: it needs per-function type information the
    // parser doesn't have) for where both are resolved and the concrete
    // synthesized class this literal constructs is determined.
    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_lambda_expression() {
        SourceLocation loc = current_loc();
        if (auto _r = expect(TokenKind::LBracket, "'['"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Lambda;
        node->loc = loc;

        bool first = true;
        if (!check(TokenKind::RBracket)) {
            // do-while isn't supported by the self-hosting parser (no
            // `do` keyword/AST node exists at all -- confirmed absent
            // from the whole compiler source tree, not just here), so
            // this is rewritten as an equivalent `while (true) { ... }`
            // with an explicit trailing `if (!match(...)) break;` at
            // every point the original `} while (match(TokenKind::
            // Comma));` condition would have been checked -- including
            // each `continue;` below, since a do-while's `continue`
            // jumps straight to its condition check (unlike a plain
            // `while`'s `continue`, which would otherwise skip the
            // comma check entirely and looping forever without ever
            // consuming/testing for a separator).
            while (true) {
                // A capture-default (`=`/`&`) is only meaningful as the
                // very first item -- real C++ rejects it elsewhere too,
                // but this parser simply never looks for one past the
                // first position, so a stray later `=`/`&` instead falls
                // through to an ordinary (by-value/by-reference) capture
                // parse, which naturally rejects nonsense like a second
                // bare `&` via expect(Identifier) failing.
                if (first && match(TokenKind::Assign)) {
                    node->lambda_blanket_mode = LambdaCaptureMode::ByValue;
                    first = false;
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
                if (first && check(TokenKind::Amp) &&
                    (peek_at(1).kind == TokenKind::Comma || peek_at(1).kind == TokenKind::RBracket)) {
                    advance();
                    node->lambda_blanket_mode = LambdaCaptureMode::ByReference;
                    first = false;
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
                first = false;

                if (match(TokenKind::Star)) {
                    if (auto _r = expect(TokenKind::KwThis, "'this' (after '*' in a capture)"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                    LambdaCapture capture{};

                    capture.name = "this";
                    capture.by_reference = false;
                    node->lambda_captures.push_back(std::move(capture));
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
                LambdaCapture capture{};

                if (match(TokenKind::KwThis)) {
                    // `[this]` -- a reference to the enclosing method's
                    // own receiver (this is scpp's *only* `this`
                    // representation, ch05 §5.9 -- there is no separate
                    // raw-pointer form to distinguish from).
                    capture.name = "this";
                    capture.by_reference = true;
                    node->lambda_captures.push_back(std::move(capture));
                    if (!match(TokenKind::Comma)) break;
                    continue;
                }
                capture.by_reference = match(TokenKind::Amp);
                auto capture_name_result = expect(TokenKind::Identifier, "captured variable name");
                if (!capture_name_result.has_value()) return std::unexpected(std::move(capture_name_result).error());
                capture.name = std::string(capture_name_result.value().text.data(), capture_name_result.value().text.size());
                if (match(TokenKind::Assign)) {
                    // Init-capture: `[name = expr]`/`[&name = expr]` --
                    // `expr` is evaluated in the *enclosing* scope; how
                    // a move-only type crosses into a closure (ch05
                    // §5.12), e.g. `[p = std::move(p)]`.
                    auto _tmp_result = parse_expr();
                    if (!_tmp_result.has_value()) return std::unexpected(std::move(_tmp_result).error());
                    capture.init = std::move(_tmp_result).value();
                }
                node->lambda_captures.push_back(std::move(capture));
                if (!match(TokenKind::Comma)) break;
            }
        }
        if (auto _r = expect(TokenKind::RBracket, "']'"); !_r.has_value()) return std::unexpected(std::move(_r).error());

        auto lambda_params_result = parse_param_list();
        if (!lambda_params_result.has_value()) return std::unexpected(std::move(lambda_params_result).error());
        node->lambda_params = std::move(lambda_params_result).value();
        // ch05 §5.12: real C++14 generic lambdas (a bare `auto`
        // parameter, e.g. `[](auto x) { ... }`) are supported -- only a
        // *named*-concept-constrained lambda parameter (`Shape auto`)
        // remains rejected: ch05 §5.11's "only a free function may be
        // generic" scoping is about that form specifically (it would
        // need the same per-call-site monomorphized-clone machinery a
        // full-header-form generic *function* gets, never built for a
        // lambda's own synthesized closure class), whereas a bare `auto`
        // parameter needs nothing beyond what a lambda's own call
        // already resolves through the shared "$auto" witness (see
        // parse_param_type/parse_program) -- exactly like the
        // abbreviated-form *function* case, checked once, abstractly,
        // against a synthesized witness type, with zero per-call-site
        // cloning.
        for (const Param& param : node->lambda_params) {
            if (!param.generic_concept.empty() && param.generic_concept != "$auto") {
                {
                    std::string _msg_8716{"a generic (concept-constrained) parameter is not supported on a lambda "};
                    _msg_8716 += "parameter list in this version (ch05 §5.11 -- only a free function may be ";
                    _msg_8716 += "generic); a bare 'auto' parameter is fine";
                    return std::unexpected(ParseError(current_loc().line, current_loc().column,
                                  _msg_8716));
                }
            }
        }

        node->lambda_is_mutable = match(TokenKind::KwMutable);

        if (match(TokenKind::Arrow)) {
            auto type_result = parse_type();
            if (!type_result.has_value()) return std::unexpected(std::move(type_result).error());
            node->type = std::move(type_result).value();
            node->has_lambda_explicit_return_type = true;
        }

        auto lambda_body_result = parse_block();
        if (!lambda_body_result.has_value()) return std::unexpected(std::move(lambda_body_result).error());
        node->lambda_body = std::move(lambda_body_result).value();
        return std::move(node);
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_primary() {
        nesting_depth_++;
        if (nesting_depth_ > kMaxNestingDepth) {
            nesting_depth_--;
            return std::unexpected(nesting_too_deep_error("expression"));
        }
        auto result = parse_primary_inner();
        nesting_depth_--;
        return result;
    }

    [[nodiscard]] std::expected<ExprPtr, ParseError> parse_primary_inner() {
        const Token& tok = peek();
        // current_loc() (not a bare make_source_location(tok.line,
        // tok.column)) so every primary expression this function builds
        // (identifiers, literals, std::move, sizeof/alignof, parenthesized
        // sub-expressions, ...) carries this parser's own source_path_ --
        // see cli.cppm's print_diagnostic for why a leaf Expr missing its
        // file identity makes a DataflowError/CodegenError rooted in an
        // imported file get silently mis-attributed to whichever file the
        // *entry point* happened to be.
        SourceLocation loc = current_loc();

        if (check(TokenKind::LBracket)) {
            return parse_lambda_expression();
        }

        if (check(TokenKind::ColonColon)) {
            std::string global_name = peek_global_qualified_name();
            if (global_name == "std::move") {
                auto global_name_result = parse_global_qualified_name();
                if (!global_name_result.has_value()) return std::unexpected(std::move(global_name_result).error());
                if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto inner_result = parse_expr();
                if (!inner_result.has_value()) return std::unexpected(std::move(inner_result).error());
                ExprPtr inner = std::move(inner_result).value();
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Move;
                node->loc = loc;
                node->lhs = std::move(inner);
                return std::move(node);
            }
            if (global_name == "scpp::is_thread_movable" || global_name == "scpp::is_thread_shareable") {
                bool movable = global_name == "scpp::is_thread_movable";
                auto global_name_result = parse_global_qualified_name();
                if (!global_name_result.has_value()) return std::unexpected(std::move(global_name_result).error());
                if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto queried_result = parse_type();
                if (!queried_result.has_value()) return std::unexpected(std::move(queried_result).error());
                Type queried = std::move(queried_result).value();
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::TypeTrait;
                node->loc = loc;
                node->name = movable ? "is_thread_movable" : "is_thread_shareable";
                node->type = std::move(queried);
                return std::move(node);
            }
            auto name_result = parse_global_qualified_name();
            if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
            std::string name = std::move(name_result).value();
            if (std::optional<std::string> specialized_name =
                    try_parse_template_static_member_name(name, /*explicit_global_qualification=*/true);
                specialized_name.has_value()) {
                name = std::move(specialized_name).value();
            }
            bool is_generic_fn = generic_function_template_params_.contains(name);
            std::vector<ExplicitTemplateArg> explicit_template_args{};

            if (is_generic_fn && check(TokenKind::Less)) {
                auto template_args_result = parse_explicit_template_args(generic_function_template_params_.at(name));
                if (!template_args_result.has_value()) return std::unexpected(std::move(template_args_result).error());
                explicit_template_args = std::move(template_args_result).value();
            } else {
                auto type_template_args_result = try_parse_explicit_generic_type_constructor_template_args(
                    name, /*explicit_global_qualification=*/true);
                if (!type_template_args_result.has_value()) return std::unexpected(std::move(type_template_args_result).error());
                std::optional<std::vector<ExplicitTemplateArg>> maybe_explicit_template_args = std::move(type_template_args_result).value();
                if (maybe_explicit_template_args.has_value()) {
                    explicit_template_args = std::move(maybe_explicit_template_args).value();
                }
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            if (std::optional<std::string> qualified_name =
                    resolve_value_qualified_type_owner_name(name, /*explicit_global_qualification=*/true);
                qualified_name.has_value()) {
                std::string& qualified_name_ref = *qualified_name;
                name = std::move(qualified_name_ref);
            }
            node->name = std::move(name);
            node->explicit_global_qualification = true;
            node->explicit_template_args = std::move(explicit_template_args);
            return std::move(node);
        }

        if (check_std_qualified("move")) {
            consume_std_qualified();
            if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto inner_result = parse_expr();
            if (!inner_result.has_value()) return std::unexpected(std::move(inner_result).error());
            ExprPtr inner = std::move(inner_result).value();
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Move;
            node->loc = loc;
            node->lhs = std::move(inner);
            return std::move(node);
        }

        if (check_scpp_qualified("is_thread_movable") || check_scpp_qualified("is_thread_shareable")) {
            bool movable = check_scpp_qualified("is_thread_movable");
            advance(); // scpp
            if (auto _r = expect(TokenKind::ColonColon, "'::'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            advance(); // is_thread_*
            if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto queried_result = parse_type();
            if (!queried_result.has_value()) return std::unexpected(std::move(queried_result).error());
            Type queried = std::move(queried_result).value();
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::TypeTrait;
            node->loc = loc;
            node->name = movable ? "is_thread_movable" : "is_thread_shareable";
            node->type = std::move(queried);
            return std::move(node);
        }

        if (match(TokenKind::KwNew)) {
            ExprPtr placement{};

            if (match(TokenKind::LParen)) {
                auto placement_result = parse_expr();
                if (!placement_result.has_value()) return std::unexpected(std::move(placement_result).error());
                placement = std::move(placement_result).value();
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            auto element_type_result = parse_type();
            if (!element_type_result.has_value()) return std::unexpected(std::move(element_type_result).error());
            Type element_type = std::move(element_type_result).value();
            if (match(TokenKind::LBracket)) {
                if (auto _r = expect(TokenKind::RBracket, "']' after 'new T['"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                {
                    std::string _msg_8869{"'new T[n]' is not supported in this version yet; only scalar/object 'new T' "};
                    _msg_8869 += "and 'new T(args...)' are implemented";
                    return std::unexpected(ParseError(loc.line, loc.column,
                                  _msg_8869));
                }
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::New;
            node->loc = loc;
            node->type = std::move(element_type);
            node->lhs = std::move(placement);
            if (match(TokenKind::LParen)) {
                node->has_paren_init = true;
                if (!check(TokenKind::RParen)) {
                    while (true) {
                        auto arg_result = parse_expr();
                        if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                        ExprPtr __arg_result_value = std::move(arg_result).value();
                        node->args.push_back(std::move(__arg_result_value));
                        if (!(match(TokenKind::Comma))) break;
                    }
                }
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            }
            return std::move(node);
        }

        // ch06 §6: `static_cast<T>(expr)` -- real C++ keyword syntax
        // verbatim (a core-language cast, unlike make_unique/move above,
        // so never `std::`-qualified). The only other spelling for an
        // explicit scalar-to-scalar conversion is the C-style cast
        // `(T)expr` -- see parse_unary's own handling of that (ambiguous
        // with a parenthesized expression, so resolved there instead,
        // by speculative parsing).
        if (check(TokenKind::Identifier) && peek().text == "static_cast" && peek_at(1).kind == TokenKind::Less) {
            advance(); // 'static_cast'
            advance(); // '<'
            auto target_type_result = parse_type();
            if (!target_type_result.has_value()) return std::unexpected(std::move(target_type_result).error());
            Type target_type = std::move(target_type_result).value();
            if (auto _r = expect(TokenKind::Greater, "'>'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            if (auto _r = expect(TokenKind::LParen, "'('"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto operand_result = parse_expr();
            if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
            ExprPtr operand = std::move(operand_result).value();
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Cast;
            node->loc = loc;
            node->type = std::move(target_type);
            node->lhs = std::move(operand);
            return std::move(node);
        }

        if (match(TokenKind::IntegerLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::IntegerLiteral;
            node->loc = loc;
            std::int64_t parsed_int_value = 0;
            // spec §5.1(5.1): `data() + size()` is pointer arithmetic on
            // a raw `const char*`. `from_chars` only offers the
            // first/last pointer-pair form, so the one-past-the-end
            // pointer has to be formed here.
            [[scpp::unsafe]] {
                std::from_chars(tok.text.data(), tok.text.data() + tok.text.size(), parsed_int_value);
            }
            node->int_value = parsed_int_value;
            return std::move(node);
        }
        if (match(TokenKind::FloatLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::FloatLiteral;
            node->loc = loc;
            double parsed_float_value = 0.0;
            // Same one-past-the-end pointer as the integer case above.
            [[scpp::unsafe]] {
                std::from_chars(tok.text.data(), tok.text.data() + tok.text.size(), parsed_float_value);
            }
            node->float_value = parsed_float_value;
            return std::move(node);
        }
        if (match(TokenKind::CharLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::CharLiteral;
            node->loc = loc;
            auto int_value_result = decode_char_literal(tok);
            if (!int_value_result.has_value()) return std::unexpected(std::move(int_value_result).error());
            node->int_value = int_value_result.value();
            return std::move(node);
        }
        if (match(TokenKind::StringLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::StringLiteral;
            node->loc = loc;
            auto name_result = decode_adjacent_string_literals(tok);
            if (!name_result.has_value()) return std::unexpected(std::move(name_result).error());
            node->name = std::move(name_result).value();
            return std::move(node);
        }
        if (match(TokenKind::KwThis)) {
            // ch05 §5.9: `this` is a keyword (not an ordinary identifier
            // -- so a user can never accidentally shadow it with a
            // same-named parameter/local), but behaves exactly like an
            // Identifier expression bound to the name "this" everywhere
            // downstream: it is resolved to a declaration by mir.cppm's
            // resolve_locals, and looked up through codegen's `locals_`,
            // exactly like any other reference-typed local, since
            // parse_class_def's make_this_param already registered it as
            // an ordinary params[0] named "this".
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            node->name = "this";
            return std::move(node);
        }
        if (match(TokenKind::KwTrue)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = true;
            return std::move(node);
        }
        if (match(TokenKind::KwFalse)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = false;
            return std::move(node);
        }
        if (match(TokenKind::KwNullptr)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::NullptrLiteral;
            node->loc = loc;
            return std::move(node);
        }
        if (check(TokenKind::Identifier)) {
            // ch11: may be a plain name or a namespace-qualified one
            // (`std::string`, `a::b::foo`) -- parse_qualified_name
            // consumes the whole `::`-joined chain (a lone identifier,
            // the overwhelmingly common case, is just a chain of length
            // one).
            std::string name = parse_qualified_name();
            if (std::optional<std::string> specialized_name =
                    try_parse_template_static_member_name(name, /*explicit_global_qualification=*/false);
                specialized_name.has_value()) {
                name = std::move(specialized_name).value();
            }
            // ch05 §5.11: `name<Args>(...)` -- an explicit-template-
            // argument call to a known full-header-form generic
            // function (e.g. `make<Circle>()`, `get<2>(t)`) --
            // recognized structurally only when `name` is already a
            // declared generic-function-template name (mirrors how a
            // generic *type* instantiation, `Name<Arg>`, is
            // disambiguated in parse_unqualified_type), avoiding any
            // ambiguity with an ordinary `a < b` comparison for every
            // other identifier.
            bool is_generic_fn = generic_function_template_params_.contains(name);
            std::vector<ExplicitTemplateArg> explicit_template_args{};

            if (is_generic_fn && check(TokenKind::Less)) {
                auto template_args_result = parse_explicit_template_args(generic_function_template_params_.at(name));
                if (!template_args_result.has_value()) return std::unexpected(std::move(template_args_result).error());
                explicit_template_args = std::move(template_args_result).value();
            } else {
                auto type_template_args_result = try_parse_explicit_generic_type_constructor_template_args(
                    name, /*explicit_global_qualification=*/false);
                if (!type_template_args_result.has_value()) return std::unexpected(std::move(type_template_args_result).error());
                std::optional<std::vector<ExplicitTemplateArg>> maybe_explicit_template_args = std::move(type_template_args_result).value();
                if (maybe_explicit_template_args.has_value()) {
                    explicit_template_args = std::move(maybe_explicit_template_args).value();
                }
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            if (std::optional<std::string> qualified_name =
                    resolve_value_qualified_type_owner_name(name, /*explicit_global_qualification=*/false);
                qualified_name.has_value()) {
                std::string& qualified_name_ref2 = *qualified_name;
                name = std::move(qualified_name_ref2);
            }
            node->name = name;
            node->explicit_template_args = std::move(explicit_template_args);
            return std::move(node);
        }
        if (match(TokenKind::LParen)) {
            // A '(' begins one of three things: a left fold `(... op pack)`,
            // a right fold `(pack op ...)`, or an ordinary parenthesised
            // expression. Only the folds contain a '...' directly inside
            // these parentheses, and that is decidable from the token
            // stream alone, so decide it here rather than by parsing a
            // candidate and rewinding.
            //
            // Rewinding is what this did, and it cost exponential time.
            // Detecting the right fold meant running a full parse_unary()
            // over the parenthesised contents, then throwing the result
            // away and parsing the same tokens again with parse_expr().
            // Each nesting level therefore parsed its interior twice, so
            // the innermost token of `((((...1...))))` was visited 2^depth
            // times -- measured at exactly 2^depth: 256 visits at depth 8,
            // 262,144 at depth 18, with 24 levels taking 42 seconds to
            // parse. The scan below is linear in the tokens it skips and
            // builds no AST, so the ordinary case now parses once.
            if (!parenthesized_group_contains_fold_ellipsis()) {
                auto inner_result = parse_expr();
                if (!inner_result.has_value()) return std::unexpected(std::move(inner_result).error());
                ExprPtr inner = std::move(inner_result).value();
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return std::move(inner);
            }
            std::size_t saved_pos = pos_;
            if (match(TokenKind::Ellipsis)) {
                std::optional<BinaryOp> op = parse_fold_operator();
                if (!op.has_value()) {
                    const Token& bad = peek();
                    return std::unexpected(ParseError(bad.line, bad.column,
                                      "expected a fold operator after '...' (ch05 §5.11)"));
                }
                auto pack_result = parse_unary();
                if (!pack_result.has_value()) return std::unexpected(std::move(pack_result).error());
                ExprPtr pack = std::move(pack_result).value();
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Fold;
                node->loc = loc;
                node->binary_op = *op;
                node->fold_ellipsis_on_left = true;
                node->lhs = std::move(pack);
                return std::move(node);
            }
            pos_ = saved_pos;
            auto first_result = parse_unary();
            if (!first_result.has_value()) return std::unexpected(std::move(first_result).error());
            ExprPtr first = std::move(first_result).value();
            if (std::optional<BinaryOp> op = parse_fold_operator(); op.has_value() && check(TokenKind::Ellipsis)) {
                advance(); // '...'
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Fold;
                node->loc = loc;
                node->binary_op = *op;
                node->lhs = std::move(first);
                if (std::optional<BinaryOp> trailing = parse_fold_operator(); trailing.has_value()) {
                    if (*trailing != *op) {
                        const Token& bad = peek();
                        {
                            std::string _msg_9065{"a binary fold expression must use the same operator on both sides of "};
                            _msg_9065 += "'...' (ch05 §5.11)";
                            return std::unexpected(ParseError(bad.line, bad.column,
                                          _msg_9065));
                        }
                    }
                    auto rhs_result = parse_unary();
                    if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                    node->rhs = std::move(rhs_result).value();
                }
                if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
                return std::move(node);
            }
            pos_ = saved_pos;
            auto inner_result = parse_expr();
            if (!inner_result.has_value()) return std::unexpected(std::move(inner_result).error());
            ExprPtr inner = std::move(inner_result).value();
            if (auto _r = expect(TokenKind::RParen, "')'"); !_r.has_value()) return std::unexpected(std::move(_r).error());
            return std::move(inner);
        }

        {
            std::string _msg_9083{"expected an expression but found '"};
            _msg_9083 += std::string(tok.text.data(), tok.text.size());
            _msg_9083 += "'";
            return std::unexpected(ParseError(tok.line, tok.column, _msg_9083));
        }
    }

    static ExprPtr make_binary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Binary;
        node->binary_op = op;
        node->loc = lhs->loc;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }

    // True if a '...' appears directly inside the parenthesised group that
    // starts at pos_ (the '(' having already been consumed), i.e. at nesting
    // depth zero within it. Both fold forms, `(... op pack)` and
    // `(pack op ...)`, spell their ellipsis there; a '...' belonging to
    // something nested, as in `(f(xs...))`, sits at a deeper level and is
    // correctly not reported.
    //
    // This is a conservative over-approximation on purpose: it answers
    // "could this be a fold?", not "is it one". A group containing a
    // top-level '...' that turns out not to be a fold still falls through
    // to the parse-and-rewind path below and is parsed exactly as before,
    // so no input changes meaning -- only inputs that cannot be folds skip
    // the speculative parse, which is what makes the common case linear.
    [[nodiscard]] bool parenthesized_group_contains_fold_ellipsis() const {
        int depth = 0;
        std::size_t scan = pos_;
        while (scan < tokens_.size()) {
            TokenKind kind = tokens_[scan].kind;
            if (kind == TokenKind::EndOfFile) return false;
            if (kind == TokenKind::Ellipsis && depth == 0) return true;
            if (kind == TokenKind::LParen || kind == TokenKind::LBracket || kind == TokenKind::LBrace) {
                depth++;
            } else if (kind == TokenKind::RParen || kind == TokenKind::RBracket || kind == TokenKind::RBrace) {
                if (depth == 0) return false;
                depth--;
            }
            scan++;
        }
        return false;
    }

    std::optional<BinaryOp> parse_fold_operator() {
        if (match(TokenKind::Plus)) { BinaryOp op = BinaryOp::Add; return op; }
        if (match(TokenKind::Minus)) { BinaryOp op = BinaryOp::Sub; return op; }
        if (match(TokenKind::Star)) { BinaryOp op = BinaryOp::Mul; return op; }
        if (match(TokenKind::Slash)) { BinaryOp op = BinaryOp::Div; return op; }
        if (match(TokenKind::EqualEqual)) { BinaryOp op = BinaryOp::Eq; return op; }
        if (match(TokenKind::NotEqual)) { BinaryOp op = BinaryOp::Ne; return op; }
        if (match(TokenKind::Less)) { BinaryOp op = BinaryOp::Lt; return op; }
        if (match(TokenKind::Greater)) { BinaryOp op = BinaryOp::Gt; return op; }
        if (match(TokenKind::LessEqual)) { BinaryOp op = BinaryOp::Le; return op; }
        if (match(TokenKind::GreaterEqual)) { BinaryOp op = BinaryOp::Ge; return op; }
        if (match(TokenKind::AmpAmp)) { BinaryOp op = BinaryOp::And; return op; }
        if (match(TokenKind::PipePipe)) { BinaryOp op = BinaryOp::Or; return op; }
        return std::optional<BinaryOp>{};
    }
};

// ch11 §11.7/§11.8: every recursive, per-file parse (the entry file, and
// -- via ModuleCache::resolve/resolve_partition in driver.cppm -- each
// `--import name=path` file and same-module partition) funnels through
// this one function, one Parser instance per file. That makes it the
// single choke point where a ParseError, freshly produced from anywhere
// inside this file's own parse_program() with no file identity of its
// own yet (see ParseError's own comment), can be stamped with *this*
// call's file before it propagates further -- but only if it doesn't
// already have one: a ParseError produced while parsing an imported file
// already passed through *that* file's own parse() frame (deeper in the
// call stack, since resolving an `import` recurses into parse() before
// this frame's own check below runs) and was stamped there already, so
// this frame must leave it alone and simply let it keep propagating,
// unchanged, out to whichever file actually needed it.
//
// Returns std::expected rather than throwing (this file's own public API
// boundary for the exception-free conversion described in ParseError's
// comment): every caller of this function -- driver.cppm's
// ModuleCache::resolve/resolve_partition and its own
// emit_object_file/emit_module_artifacts/compile_to_executable, cli.cppm's
// run_parse, and every test file that parses source directly -- now
// checks `.has_value()` instead of using try/catch.
[[nodiscard]] std::expected<Program, ParseError> parse(std::vector<Token> tokens, const ModuleResolver& resolver = no_module_resolver,
              const PartitionResolver& partition_resolver = no_partition_resolver, std::string source_path = {}) {
    Parser parser{std::move(tokens), resolver, partition_resolver, std::move(source_path)};
    std::expected<Program, ParseError> result = parser.parse_program();
    if (!result.has_value() && !result.error().loc.has_source_path()) {
        result.error().loc.source_path = parser.source_path();
    }
    return result;
}

[[nodiscard]] std::expected<Program, ParseError> parse(std::string_view source, const ModuleResolver& resolver = no_module_resolver,
              const PartitionResolver& partition_resolver = no_partition_resolver, std::string source_path = {}) {
    return parse(tokenize(source), resolver, partition_resolver, std::move(source_path));
}

} // namespace scpp
