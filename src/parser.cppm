module;

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

export module scpp.parser;

import scpp.lexer;
import scpp.ast;

export namespace scpp {

struct ParseError : std::runtime_error {
    ParseError(int line, int column, const std::string& message)
        : std::runtime_error(message), line(line), column(column), loc{line, column} {}
    int line;
    int column;
    // Same position as line/column above, just packaged as a
    // SourceLocation (ast.cppm) so cli.cppm's diagnostic printer can
    // treat every error kind (Parse/Dataflow/Codegen) uniformly.
    SourceLocation loc;
};

// ch11 §11.8: given a module's dotted name (e.g. "std"), returns a
// reference to that module's already-parsed (and, transitively, already
// import-resolved) Program -- called while parsing an `import name;`
// declaration, so the imported module's exported struct/class names are
// registered (struct_names_/class_names_) before the rest of the
// importing file is parsed (mirrors real C++20: imports must precede
// every other declaration). Owned and cached by the driver (which knows
// about `--import name=path` mappings and file I/O -- the parser itself
// never touches the filesystem); throws if `name` has no known mapping.
// Left default-constructed (empty std::function) for any caller with no
// imports to resolve -- never actually invoked unless the source being
// parsed contains a real `import` declaration, so every existing
// import-free caller (the whole test suite, today) is unaffected.
using ModuleResolver = std::function<const Program&(const std::string&)>;

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
// needed cross-partition identity (nothing in v1 does). Left default-
// constructed for any caller with no partitions to resolve.
using PartitionResolver = std::function<Program(const std::string&)>;

class Parser {
public:
    explicit Parser(std::vector<Token> tokens, ModuleResolver resolver = {},
                     PartitionResolver partition_resolver = {})
        : tokens_(std::move(tokens)), resolver_(std::move(resolver)),
          partition_resolver_(std::move(partition_resolver)) {
        // ch06 §6: the numeric family's own non-keyword members -- real
        // C++ <cstdint>/<cstddef>/<stdfloat> typedef names, not keywords
        // at all (unlike int/bool/char/long/float/double/unsigned,
        // recognized directly by parse_unqualified_type's own keyword
        // chain instead) -- pre-registered here exactly like an
        // already-declared struct/class name (struct_names_ is what
        // looks_like_type_start/parse_unqualified_type's own Identifier
        // fallback both already consult), so every one of these is
        // recognized as a type name from the very first line of every
        // program, unconditionally guaranteed on every target (ch06's
        // own "no platform on which scpp would need to omit any of
        // these" rationale) -- never registered in class_names_ (they're
        // scalars, not classes: no access control, no method-call
        // machinery, no by-value-parameter restriction).
        for (const char* name : {"int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                                  "uint64_t", "size_t", "ptrdiff_t", "float32_t", "float64_t"}) {
            struct_names_.insert(name);
        }
    }

    Program parse_program() {
        Program program;
        parse_module_declaration(program);
        parse_import_declarations(program);
        parse_top_level_items(program);
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
            program.classes.push_back(ClassDef{.name = "$auto", .is_concept_witness = true});
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
            program.concepts.push_back(ConceptDef{.name = "$auto"});
        }
        return program;
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    ModuleResolver resolver_;
    PartitionResolver partition_resolver_;
    // ch11 §11.4: the namespace path currently being parsed into, e.g.
    // inside `namespace std { ... }` this is {"std"}; empty at file
    // scope (today's default -- every existing, non-namespaced file is
    // unaffected). Pushed/popped around a namespace block
    // (parse_namespace_block); qualify_name() joins this onto a bare
    // declared name to produce the fully-qualified form used as that
    // declaration's actual `.name` (see struct_names_ below).
    std::vector<std::string> namespace_stack_;
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
    std::unordered_set<std::string> struct_names_;
    // Class names specifically (ch04 §4.2) -- see struct_names_ above for
    // why this is a second, narrower set rather than the only one.
    std::unordered_set<std::string> class_names_;
    // ch05 §5.11: concept names, keyed the same fully-qualified way as
    // struct_names_/class_names_. Consulted by generic-parameter parsing
    // (`ConceptName auto& name`) to recognize the abbreviated generic-
    // function form: an identifier immediately followed by `auto` is
    // only treated as a concept-constrained parameter when it names an
    // already-declared concept (concepts, like every other declaration
    // this parser handles, must be declared before use).
    std::unordered_set<std::string> concept_names_;
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
    std::unordered_set<std::string> generic_type_names_;
    // ch05 §5.14: every variadic primary template's own declared
    // parameter list (`template<typename... Ts> class Tuple;`), keyed
    // by its qualified name -- consulted by parse_variadic_specialization
    // to validate a later specialization's own `<...>` argument list
    // actually matches one of the two fixed patterns (an empty pack, or
    // exactly the primary template's own parameter names in order), and
    // to recognize the pack parameter's own name (needed for a
    // specialization's `: private Tuple<Tail...>` base-clause, see
    // ClassDef::base_pack_arg_name).
    std::unordered_map<std::string, std::vector<GenericTypeParam>> variadic_primary_template_params_;
    // ch05 §5.14: every ordinary (non-variadic) generic class/struct's
    // own declared template parameter list, keyed by its qualified name
    // -- consulted at an instantiation site (parse_unqualified_type's
    // generic-type-argument loop below) to know how many arguments are
    // expected and, for each position, whether it is a type argument or
    // a non-type one parsed as an expression into Type::non_type_args.
    // Mixed ordinary templates interleaving type and non-type
    // parameters would need the Type AST to preserve argument order
    // rather than today's split template_args/non_type_args storage, so
    // ordinary generic classes/structs are currently limited to an
    // all-type or all-non-type parameter list; variadic_primary_
    // template_params_ above already handles the separate recursive-
    // inheritance variadic family.
    std::unordered_map<std::string, std::vector<GenericTypeParam>> ordinary_generic_type_template_params_;
    // ch05 §5.11: every full-header-form generic function's own declared
    // template parameter list (`template<size_t I, typename Head,
    // typename... Tail> Head& get(...)`), keyed by its qualified name --
    // consulted at a *call* site (parse_postfix's Identifier-then-`(`
    // handling) to recognize `name<Args>(...)` as an explicit-template-
    // argument call (rather than misparsing `<`/`>` as comparison
    // operators, the classic ambiguity) and to know, for each argument
    // position, whether to parse a type or a non-type expression.
    std::unordered_map<std::string, std::vector<GenericTypeParam>> generic_function_template_params_;
    // Non-empty only while parsing one full-header-form generic function's
    // signature/body (`template<...> ReturnType name(...) { ... }`). Lets the
    // ordinary parameter parser recognize `Args... args` as a real template
    // parameter pack rather than rejecting every non-concept pack as the
    // abbreviated-generic-only form.
    std::vector<GenericTypeParam> current_function_template_params_;
    // Non-empty only while parsing the body/signature surface of one
    // generic class/specialization. Lets member parameter parsing and
    // function-pointer declarators recognize named pack parameters from
    // the enclosing type template as real pack expansions.
    std::vector<GenericTypeParam> current_class_template_params_;

    [[nodiscard]] const Token& peek() const { return tokens_[pos_]; }
    [[nodiscard]] bool check(TokenKind kind) const { return peek().kind == kind; }

    // The position of the *next* token to be consumed -- called at the
    // start of parsing a new Expr/Stmt/Function, before any of its own
    // tokens are consumed, so the resulting node's `.loc` points at
    // wherever it syntactically begins (see SourceLocation, ast.cppm).
    [[nodiscard]] SourceLocation current_loc() const { return SourceLocation{peek().line, peek().column}; }

    const Token& advance() {
        const Token& tok = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) pos_++;
        return tok;
    }

    bool match(TokenKind kind) {
        if (!check(kind)) return false;
        advance();
        return true;
    }

    const Token& expect(TokenKind kind, const std::string& what) {
        if (!check(kind)) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column, "expected " + what + " but found '" +
                                                        std::string(tok.text) + "'");
        }
        return advance();
    }

    [[nodiscard]] bool looks_like_type_start() const {
        const Token& tok = peek();
        if (tok.kind == TokenKind::KwInt || tok.kind == TokenKind::KwBool || tok.kind == TokenKind::KwConst ||
            tok.kind == TokenKind::KwVoid || tok.kind == TokenKind::KwChar || tok.kind == TokenKind::KwLong ||
            tok.kind == TokenKind::KwFloat || tok.kind == TokenKind::KwDouble ||
            tok.kind == TokenKind::KwUnsigned) {
            return true;
        }
        // ch05 §5.12: a bare `auto` at statement start unambiguously
        // means an auto-typed var-decl (`auto f = expr;`) -- the only
        // way to name a closure's own compiler-synthesized, otherwise
        // unspellable type. `auto`'s *other* legal appearance (`Concept
        // auto` in parameter position, parse_param_type) is never the
        // very first token of a statement, so there's no ambiguity here.
        if (tok.kind == TokenKind::KwAuto) return true;
        if (check_std_qualified("span")) return true;
        if (tok.kind != TokenKind::Identifier) return false;
        // ch11: a bare identifier might be the *first segment* of a
        // qualified name (`std::string`) rather than a plain type name --
        // peek_qualified_name looks ahead through the whole `::` chain so
        // the fully-qualified form is what gets checked against
        // struct_names_ (which registers declarations under exactly that
        // form -- see parse_class_def/parse_struct_def).
        return struct_names_.contains(peek_qualified_name());
    }

    // ch06 §6: a simplified, offset-based variant of looks_like_type_
    // start above -- no std::-qualified-name lookahead (a cast's own
    // target type is never realistically std::unique_ptr<T>/
    // std::span<T> in this version) -- used only by parse_unary's own
    // C-style-cast lookahead, `(T)expr`, which needs to peek past the
    // `(` at `offset` positions ahead without disturbing `pos_` at all
    // (unlike looks_like_type_start, always checked at the current
    // position).
    [[nodiscard]] bool looks_like_type_start_at(size_t offset) const {
        const Token& tok = peek_at(offset);
        if (tok.kind == TokenKind::KwInt || tok.kind == TokenKind::KwBool || tok.kind == TokenKind::KwChar ||
            tok.kind == TokenKind::KwLong || tok.kind == TokenKind::KwFloat || tok.kind == TokenKind::KwDouble ||
            tok.kind == TokenKind::KwUnsigned) {
            return true;
        }
        return tok.kind == TokenKind::Identifier && struct_names_.contains(std::string(tok.text));
    }

    // Bounds-safe lookahead: returns the token `offset` positions ahead of
    // the current one, or the (always-last) EndOfFile token if that would
    // run past the end of the stream.
    [[nodiscard]] const Token& peek_at(size_t offset) const {
        size_t idx = pos_ + offset;
        return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
    }

    [[nodiscard]] std::optional<std::string> referenced_pack_type_param_name(const Type& type) const {
        const Type* current = &type;
        if (current->kind == TypeKind::Reference && current->pointee) current = current->pointee.get();
        if (current->kind != TypeKind::Named || !current->template_args.empty() || !current->non_type_args.empty()) {
            return std::nullopt;
        }
        for (const GenericTypeParam& param : current_class_template_params_) {
            if (!param.is_pack || param.is_non_type) continue;
            if (param.name == current->name) return param.name;
        }
        for (const GenericTypeParam& param : current_function_template_params_) {
            if (!param.is_pack || param.is_non_type) continue;
            if (param.name == current->name) return param.name;
        }
        return std::nullopt;
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
    [[nodiscard]] size_t offset_after_matching_angle(size_t less_than_offset) const {
        size_t offset = less_than_offset + 1;
        int depth = 1;
        while (depth > 0 && peek_at(offset).kind != TokenKind::EndOfFile) {
            if (peek_at(offset).kind == TokenKind::Less) depth++;
            else if (peek_at(offset).kind == TokenKind::Greater) depth--;
            offset++;
        }
        return offset;
    }

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
    // parser too, not just to a real downstream C++ compiler).
    struct ParsedAttributes {
        std::unordered_set<std::string> scpp_tokens;
        ExprPtr thread_movable_if_movable_expr;
        ExprPtr thread_movable_if_shareable_expr;
        [[nodiscard]] bool has(const std::string& token) const { return scpp_tokens.contains(token); }
    };

    ExprPtr clone_expr_tree(const Expr& expr) {
        auto clone = std::make_unique<Expr>();
        clone->kind = expr.kind;
        clone->loc = expr.loc;
        clone->int_value = expr.int_value;
        clone->float_value = expr.float_value;
        clone->bool_value = expr.bool_value;
        clone->name = expr.name;
        clone->binary_op = expr.binary_op;
        clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
        clone->unary_op = expr.unary_op;
        clone->type = expr.type;
        clone->has_paren_init = expr.has_paren_init;
        clone->lambda_blanket_mode = expr.lambda_blanket_mode;
        clone->lambda_params = expr.lambda_params;
        clone->has_lambda_explicit_return_type = expr.has_lambda_explicit_return_type;
        clone->lambda_is_mutable = expr.lambda_is_mutable;
        if (expr.lhs) clone->lhs = clone_expr_tree(*expr.lhs);
        if (expr.rhs) clone->rhs = clone_expr_tree(*expr.rhs);
        clone->args.clear();
        for (const ExprPtr& arg : expr.args) clone->args.push_back(clone_expr_tree(*arg));
        clone->explicit_template_args.clear();
        for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
            ExplicitTemplateArg cloned = arg;
            if (arg.value) cloned.value = std::shared_ptr<Expr>(clone_expr_tree(*arg.value).release());
            clone->explicit_template_args.push_back(std::move(cloned));
        }
        clone->lambda_captures.clear();
        for (const LambdaCapture& capture : expr.lambda_captures) {
            LambdaCapture cloned;
            cloned.name = capture.name;
            cloned.by_reference = capture.by_reference;
            if (capture.init) cloned.init = clone_expr_tree(*capture.init);
            clone->lambda_captures.push_back(std::move(cloned));
        }
        clone->lambda_body.reset();
        return clone;
    }

    ClassDef clone_class_def(const ClassDef& def) {
        ClassDef clone;
        clone.name = def.name;
        clone.fields = def.fields;
        clone.namespace_path = def.namespace_path;
        clone.is_exported = def.is_exported;
        clone.owning_module = def.owning_module;
        clone.is_concept_witness = def.is_concept_witness;
        clone.template_params = def.template_params;
        clone.is_synthetic_check_only = def.is_synthetic_check_only;
        clone.base_class_name = def.base_class_name;
        clone.base_access = def.base_access;
        clone.is_variadic_primary_template = def.is_variadic_primary_template;
        clone.is_variadic_specialization = def.is_variadic_specialization;
        clone.base_pack_arg_name = def.base_pack_arg_name;
        if (def.base_non_type_arg) clone.base_non_type_arg = std::shared_ptr<Expr>(clone_expr_tree(*def.base_non_type_arg).release());
        clone.thread_movable_override = def.thread_movable_override;
        clone.thread_shareable_override = def.thread_shareable_override;
        if (def.thread_movable_if_movable_expr) {
            clone.thread_movable_if_movable_expr = clone_expr_tree(*def.thread_movable_if_movable_expr);
        }
        if (def.thread_movable_if_shareable_expr) {
            clone.thread_movable_if_shareable_expr = clone_expr_tree(*def.thread_movable_if_shareable_expr);
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
    void skip_attribute_arguments() {
        int depth = 1;
        while (depth > 0) {
            if (check(TokenKind::EndOfFile)) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column, "unterminated attribute argument list");
            }
            if (match(TokenKind::LParen)) {
                depth++;
            } else if (match(TokenKind::RParen)) {
                depth--;
            } else {
                advance();
            }
        }
    }

    [[nodiscard]] ParsedAttributes parse_attribute_specifier_seq() {
        ParsedAttributes result;
        while (check(TokenKind::LBracket) && peek_at(1).kind == TokenKind::LBracket) {
            advance(); // '['
            advance(); // '['
            if (!check(TokenKind::RBracket)) {
                do {
                    std::string ns;
                    std::string token = std::string(expect(TokenKind::Identifier, "attribute token").text);
                    if (match(TokenKind::ColonColon)) {
                        ns = token;
                        token = std::string(expect(TokenKind::Identifier, "attribute token").text);
                    }
                    if (match(TokenKind::LParen)) {
                        if (ns == "scpp" && token == "thread_movable_if") {
                            result.thread_movable_if_movable_expr = parse_expr();
                            expect(TokenKind::Comma, "','");
                            result.thread_movable_if_shareable_expr = parse_expr();
                            expect(TokenKind::RParen, "')'");
                        } else {
                            skip_attribute_arguments();
                        }
                    }
                    if (ns == "scpp") result.scpp_tokens.insert(token);
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "']'");
            expect(TokenKind::RBracket, "']'");
        }
        return result;
    }

    // Checks (without consuming) for the 3-token sequence `std :: <member>`.
    // Only the still-builtin spellings (`std::move`, plus the parser-only
    // `std::span` type form) use this helper; ordinary library names now
    // flow through the general qualified-name path.
    [[nodiscard]] bool check_std_qualified(std::string_view member) const {
        return peek().kind == TokenKind::Identifier && peek().text == "std" &&
               peek_at(1).kind == TokenKind::ColonColon && peek_at(2).kind == TokenKind::Identifier &&
               peek_at(2).text == member;
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
        std::string joined(peek().text);
        size_t offset = 1;
        while (peek_at(offset).kind == TokenKind::ColonColon && peek_at(offset + 1).kind == TokenKind::Identifier) {
            joined += "::";
            joined += peek_at(offset + 1).text;
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
        std::string joined(advance().text);
        while (check(TokenKind::ColonColon) && peek_at(1).kind == TokenKind::Identifier) {
            advance(); // ::
            joined += "::";
            joined += advance().text;
        }
        return joined;
    }

    ExprPtr clone_expr(const Expr& expr) {
        auto clone = std::make_unique<Expr>();
        clone->kind = expr.kind;
        clone->loc = expr.loc;
        clone->int_value = expr.int_value;
        clone->float_value = expr.float_value;
        clone->bool_value = expr.bool_value;
        clone->name = expr.name;
        clone->binary_op = expr.binary_op;
        if (expr.lhs) clone->lhs = clone_expr(*expr.lhs);
        if (expr.rhs) clone->rhs = clone_expr(*expr.rhs);
        clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
        clone->unary_op = expr.unary_op;
        clone->args.clear();
        for (const ExprPtr& arg : expr.args) clone->args.push_back(clone_expr(*arg));
        clone->explicit_template_args.clear();
        for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
            ExplicitTemplateArg cloned_arg = arg;
            if (arg.value) cloned_arg.value = std::shared_ptr<Expr>(clone_expr(*arg.value).release());
            clone->explicit_template_args.push_back(std::move(cloned_arg));
        }
        clone->lambda_captures.clear();
        for (const LambdaCapture& capture : expr.lambda_captures) {
            LambdaCapture cloned_capture;
            cloned_capture.name = capture.name;
            cloned_capture.by_reference = capture.by_reference;
            if (capture.init) cloned_capture.init = clone_expr(*capture.init);
            clone->lambda_captures.push_back(std::move(cloned_capture));
        }
        clone->lambda_blanket_mode = expr.lambda_blanket_mode;
        clone->lambda_params = expr.lambda_params;
        clone->has_lambda_explicit_return_type = expr.has_lambda_explicit_return_type;
        clone->lambda_is_mutable = expr.lambda_is_mutable;
        if (expr.lambda_body) clone->lambda_body = clone_stmt(*expr.lambda_body);
        clone->type = expr.type;
        clone->has_paren_init = expr.has_paren_init;
        return clone;
    }

    StmtPtr clone_stmt(const Stmt& stmt) {
        auto clone = std::make_unique<Stmt>();
        clone->kind = stmt.kind;
        clone->loc = stmt.loc;
        clone->type = stmt.type;
        clone->var_name = stmt.var_name;
        if (stmt.init) clone->init = clone_expr(*stmt.init);
        clone->has_ctor_args = stmt.has_ctor_args;
        clone->ctor_args.clear();
        for (const ExprPtr& arg : stmt.ctor_args) clone->ctor_args.push_back(clone_expr(*arg));
        if (stmt.expr) clone->expr = clone_expr(*stmt.expr);
        if (stmt.condition) clone->condition = clone_expr(*stmt.condition);
        if (stmt.then_branch) clone->then_branch = clone_stmt(*stmt.then_branch);
        if (stmt.else_branch) clone->else_branch = clone_stmt(*stmt.else_branch);
        clone->statements.clear();
        for (const StmtPtr& s : stmt.statements) clone->statements.push_back(clone_stmt(*s));
        clone->is_unsafe = stmt.is_unsafe;
        return clone;
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
        if (namespace_stack_.empty()) return bare_name;
        std::string joined;
        for (const std::string& segment : namespace_stack_) {
            joined += segment;
            joined += "::";
        }
        joined += bare_name;
        return joined;
    }

    // Splits a dotted module name ("org.lotx.cmath") into its segments
    // ({"org", "lotx", "cmath"}), so it can be compared segment-for-
    // segment against a `::`-based namespace_path (ch11 §11.5: module
    // names use '.', namespace paths use '::' -- translated one to the
    // other segment-for-segment, never string-compared directly).
    [[nodiscard]] static std::vector<std::string> split_dotted_name(const std::string& dotted) {
        std::vector<std::string> segments;
        size_t start = 0;
        while (start <= dotted.size()) {
            size_t dot = dotted.find('.', start);
            if (dot == std::string::npos) {
                segments.push_back(dotted.substr(start));
                break;
            }
            segments.push_back(dotted.substr(start, dot - start));
            start = dot + 1;
        }
        return segments;
    }

    // ch11 §11.5: the export/namespace validation pass -- purely
    // syntactic, no borrow-checker involvement. An `export`-marked
    // declaration only actually exports if its namespace_path, segment
    // for segment, starts with the enclosing module's own dotted name
    // (a *prefix* requirement, not exact-match: deeper nesting beyond
    // the module's own name, e.g. `org::lotx::cmath::trig`, is fine).
    // `export` and "lives in the required namespace" are two
    // independent, both-mandatory gates -- getting either wrong is a
    // compile error, caught here immediately rather than deferred to a
    // later pass.
    void check_export_namespace(const Program& program, bool is_exported,
                                 const std::vector<std::string>& namespace_path, SourceLocation loc,
                                 const std::string& what) const {
        if (!is_exported) return;
        if (program.module_name.empty()) {
            throw ParseError(loc.line, loc.column,
                              "'export' on " + what +
                                  " has no effect: this file has no 'export module'/'module' declaration "
                                  "(ch11 §11.3)");
        }
        std::vector<std::string> module_segments = split_dotted_name(program.module_name);
        bool starts_with_module_name = namespace_path.size() >= module_segments.size();
        for (size_t i = 0; starts_with_module_name && i < module_segments.size(); i++) {
            starts_with_module_name = namespace_path[i] == module_segments[i];
        }
        if (!starts_with_module_name) {
            throw ParseError(loc.line, loc.column,
                              "exported " + what + " must be declared inside a namespace matching this "
                              "module's own name ('" + program.module_name + "') -- ch11 §11.5");
        }
    }

    // Parses a base type name (`int`, `bool`, `std::unique_ptr<T>`, or a
    // known struct name) followed by zero or more `*` for pointer levels.
    // Array suffixes (`[N]`) are handled separately by parse_array_suffix,
    // since in C-style declarators the array size follows the *declared
    // name*, not the type. `const_qualifies_first_pointer` is set by
    // parse_type() when it saw a leading `const` immediately before this
    // call: it makes only the *innermost* (first-parsed) `*` level's
    // pointee const (`const T*`, or `const T**`'s inner pointer -- matching
    // real C++'s own reading of `const` as binding to the base type, not
    // an outer pointer level), never a later/outer one, mirroring how
    // real C++ reads `const int**` as "pointer to (pointer to const int)".
    Type parse_unqualified_type(bool const_qualifies_first_pointer = false) {
        if (check_std_qualified("span")) {
            consume_std_qualified();
            expect(TokenKind::Less, "'<'");
            // `const` here qualifies the *element* type (`std::span<const
            // T>`, a read-only view), not a reference -- so it's parsed
            // directly rather than through parse_type() (which only
            // accepts a leading `const` when followed by `&`).
            bool element_is_const = match(TokenKind::KwConst);
            Type element = parse_unqualified_type();
            expect(TokenKind::Greater, "'>'");
            Type type;
            type.kind = TypeKind::Span;
            type.pointee = std::make_shared<Type>(std::move(element));
            type.is_mutable_ref = !element_is_const;
            return type;
        }

        const Token& tok = peek();
        Type type;
        type.kind = TypeKind::Named;
        if (tok.kind == TokenKind::KwInt) {
            type.name = "int";
            advance();
        } else if (tok.kind == TokenKind::KwBool) {
            type.name = "bool";
            advance();
        } else if (tok.kind == TokenKind::KwChar) {
            type.name = "char";
            advance();
        } else if (tok.kind == TokenKind::KwLong) {
            // ch06 §6: `long` -- deliberately fixed as an alias for
            // int64_t regardless of target platform (unlike real C++'s
            // own platform-defined width), to design away the classic
            // LP64-vs-LLP64 cross-platform pitfall.
            type.name = "long";
            advance();
        } else if (tok.kind == TokenKind::KwFloat) {
            type.name = "float";
            advance();
        } else if (tok.kind == TokenKind::KwDouble) {
            type.name = "double";
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
                throw ParseError(next.line, next.column,
                                  "'unsigned' must be immediately followed by 'int' or 'long' (ch06 §6) -- the "
                                  "bare 'unsigned' shorthand is not valid scpp");
            }
        } else if (tok.kind == TokenKind::KwVoid) {
            // Valid here structurally (like int/bool) so `void*` falls
            // out of the trailing `*` loop below for free; a *bare*
            // (non-pointer) `void` is rejected downstream, not by the
            // parser -- see codegen's declare_function (parameters) and
            // VarDecl codegen (locals). A `void` return type is always
            // fine and needs no rejection anywhere.
            type.name = "void";
            advance();
        } else if (tok.kind == TokenKind::Identifier && generic_type_names_.contains(peek_qualified_name())) {
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
            type.name = parse_qualified_name();
            bool is_variadic = variadic_primary_template_params_.contains(type.name);
            const std::vector<GenericTypeParam>* ordinary_params = nullptr;
            auto ordinary_it = ordinary_generic_type_template_params_.find(type.name);
            if (ordinary_it != ordinary_generic_type_template_params_.end()) ordinary_params = &ordinary_it->second;
            size_t leading_non_type_count = 0;
            if (is_variadic) {
                for (const GenericTypeParam& p : variadic_primary_template_params_[type.name]) {
                    if (!p.is_non_type) break;
                    leading_non_type_count++;
                }
            }
            expect(TokenKind::Less, "'<'");
            size_t arg_index = 0;
            if (!check(TokenKind::Greater)) {
                do {
                    bool parse_non_type_arg =
                        is_variadic ? arg_index < leading_non_type_count
                                    : (ordinary_params != nullptr && arg_index < ordinary_params->size() &&
                                       (*ordinary_params)[arg_index].is_non_type);
                    if (parse_non_type_arg) {
                        type.non_type_args.push_back(std::shared_ptr<Expr>(parse_additive().release()));
                    } else if (is_variadic && check(TokenKind::Identifier) && peek_at(1).kind == TokenKind::Ellipsis) {
                        Type spread;
                        spread.kind = TypeKind::Named;
                        spread.name = std::string(advance().text);
                        advance(); // '...'
                        spread.is_pack_expansion = true;
                        type.template_args.push_back(std::move(spread));
                    } else {
                        type.template_args.push_back(parse_type());
                    }
                    arg_index++;
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::Greater, "'>'");
            if (!is_variadic && ordinary_params != nullptr) {
                size_t expected_non_type_args = 0;
                for (const GenericTypeParam& p : *ordinary_params) {
                    if (p.is_non_type) expected_non_type_args++;
                }
                size_t expected_type_args = ordinary_params->size() - expected_non_type_args;
                if (type.template_args.size() != expected_type_args || type.non_type_args.size() != expected_non_type_args) {
                    throw ParseError(name_tok.line, name_tok.column,
                                      "'" + type.name + "' takes exactly " + std::to_string(ordinary_params->size()) +
                                          " template argument(s) in this order (ch05 §5.14)");
                }
            }
        } else if (tok.kind == TokenKind::Identifier && struct_names_.contains(peek_qualified_name())) {
            type.name = parse_qualified_name();
        } else {
            throw ParseError(tok.line, tok.column, "expected a type name");
        }

        bool first_star = true;
        while (match(TokenKind::Star)) {
            auto pointee = std::make_shared<Type>(type);
            type = Type{};
            type.kind = TypeKind::Pointer;
            type.pointee = std::move(pointee);
            type.is_mutable_pointee = !(first_star && const_qualifies_first_pointer);
            first_star = false;
        }
        return type;
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
    // A bare `const T` (no `&`/`&&`/`*` at all) is rejected *unless* the
    // caller opts in via `out_bare_const` (non-null): only
    // parse_var_decl does, for a `const`-qualified local variable (spec
    // ch05/ch06 -- an immutable local, distinct from a borrow/pointer's
    // own, already-tracked read-only-ness) -- every other caller
    // (a parameter, struct/class field, return type, or nested type
    // argument) leaves this null and keeps the original rejection,
    // since scpp has no other const-qualification for those yet.
    Type parse_type(bool allow_rvalue_ref = false, bool* out_bare_const = nullptr) {
        bool has_const_prefix = match(TokenKind::KwConst);
        Type type = parse_unqualified_type(/*const_qualifies_first_pointer=*/has_const_prefix);

        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = !has_const_prefix;
            return type;
        }

        if (match(TokenKind::AmpAmp)) {
            if (!allow_rvalue_ref) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "'&&' (rvalue reference) is only supported for a function/method/"
                                  "constructor parameter's declared type in this version (ch03) -- not "
                                  "a variable, field, return type, or nested type argument");
            }
            if (has_const_prefix) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "'const' cannot qualify an rvalue reference ('const T&&') -- an "
                                  "rvalue-reference parameter always takes ownership via move (ch03), "
                                  "which needs mutable access to the value being moved from");
            }
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = true;
            type.is_rvalue_ref = true;
            return type;
        }

        if (has_const_prefix && type.kind != TypeKind::Pointer) {
            if (out_bare_const != nullptr) {
                *out_bare_const = true;
                return type;
            }
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "'const' is only supported directly before a reference type ('const T&') "
                              "or a pointer type ('const T*') in this version");
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
    Type parse_param_type(std::string& out_generic_concept) {
        out_generic_concept.clear();
        size_t const_offset = check(TokenKind::KwConst) ? 1 : 0;
        bool next_is_identifier_then_auto =
            peek_at(const_offset).kind == TokenKind::Identifier && peek_at(const_offset + 1).kind == TokenKind::KwAuto;
        bool next_is_bare_auto = peek_at(const_offset).kind == TokenKind::KwAuto;
        if (!next_is_identifier_then_auto && !next_is_bare_auto) return parse_type(/*allow_rvalue_ref=*/true);

        std::string concept_name;
        if (next_is_identifier_then_auto) {
            concept_name = std::string(peek_at(const_offset).text);
            if (!concept_names_.contains(concept_name)) {
                const Token& tok = peek_at(const_offset);
                throw ParseError(tok.line, tok.column,
                                  "'" + concept_name +
                                      "' is not a declared concept -- 'Name auto' is only legal when 'Name' names a "
                                      "concept, declared before use (ch05 §5.11)");
            }
        } else {
            concept_name = "$auto";
            bare_auto_used_ = true;
        }
        // Only for error messages below -- shows the source spelling
        // ("auto") rather than the internal "$auto" witness-class name a
        // bare parameter is recorded under.
        std::string display_name = next_is_identifier_then_auto ? concept_name : std::string("auto");
        bool has_const = match(TokenKind::KwConst);
        if (next_is_identifier_then_auto) advance(); // the concept name itself
        expect(TokenKind::KwAuto, "'auto'");
        out_generic_concept = concept_name;

        Type type;
        type.kind = TypeKind::Named;
        type.name = concept_name; // the witness class shares the concept's own name

        if (match(TokenKind::AmpAmp)) {
            if (has_const) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "'const' cannot qualify an rvalue reference ('const " + display_name +
                                      " auto&&') -- an rvalue-reference parameter always takes ownership via "
                                      "move (ch03), which needs mutable access to the value being moved from");
            }
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = true;
            type.is_rvalue_ref = true;
            return type;
        }
        if (match(TokenKind::Amp)) {
            auto pointee = std::make_shared<Type>(std::move(type));
            type = Type{};
            type.kind = TypeKind::Reference;
            type.pointee = std::move(pointee);
            type.is_mutable_ref = !has_const;
            return type;
        }
        if (has_const) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "'const' is only supported directly before a reference type ('const " +
                                  display_name + " auto&') in this version");
        }
        return type; // bare "ConceptName auto"/"auto" -- by value
    }

    // Wraps `base` in Array types for each trailing `[N]` found after a
    // declared name (e.g. the `[8]` in `int values[8];`). Arrays of
    // references aren't valid C++ (there's no storage layout for a raw
    // reference), so reject up front rather than let it silently codegen
    // as an array of addresses.
    Type parse_array_suffix(Type base) {
        // ch00 §2/ch01 §1.3: `[[` (a doubled bracket) starts an
        // attribute-specifier-seq, never an array declarator -- stop
        // here rather than misparsing e.g. `T&& f [[scpp::thread_movable]]`
        // as if `[[scpp::thread_movable]]` were an (invalid, since `f`'s
        // own type is a Reference) array-of-references suffix. A real
        // array declarator's own size is always a single `[` (never
        // doubled), so this check never rejects a legitimate one.
        while (check(TokenKind::LBracket) && peek_at(1).kind != TokenKind::LBracket) {
            const Token& bracket_tok = peek();
            if (base.kind == TypeKind::Reference) {
                throw ParseError(bracket_tok.line, bracket_tok.column, "arrays of references are not supported");
            }
            advance();
            const Token& size_tok = expect(TokenKind::IntegerLiteral, "array size");
            expect(TokenKind::RBracket, "']'");
            auto element = std::make_shared<Type>(base);
            base = Type{};
            base.kind = TypeKind::Array;
            base.element = std::move(element);
            base.array_size = std::stoll(std::string(size_tok.text));
        }
        return base;
    }

    [[nodiscard]] bool starts_function_pointer_declarator() const {
        return check(TokenKind::LParen) && peek_at(1).kind == TokenKind::Star;
    }

    std::vector<Type> parse_function_pointer_param_types() {
        std::vector<Type> params;
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                Type param_type = parse_type(/*allow_rvalue_ref=*/true);
                if (match(TokenKind::Ellipsis)) {
                    if (!referenced_pack_type_param_name(param_type).has_value()) {
                        const Token& tok = peek();
                        throw ParseError(tok.line, tok.column,
                                          "a function-pointer parameter pack must name an enclosing type or "
                                          "function template parameter pack");
                    }
                    param_type.is_pack_expansion = true;
                    if (check(TokenKind::Identifier)) advance(); // optional parameter name, ignored in a function type
                    if (!check(TokenKind::RParen)) {
                        const Token& tok = peek();
                        throw ParseError(tok.line, tok.column,
                                          "a function-pointer parameter pack must be the last parameter in the list");
                    }
                } else if (check(TokenKind::Identifier)) {
                    advance(); // optional parameter name, ignored in a function type
                }
                param_type = parse_array_suffix(param_type);
                if (param_type.kind == TypeKind::Array) {
                    Type decayed;
                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                params.push_back(std::move(param_type));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        return params;
    }

    Type parse_function_pointer_declarator(Type return_type, std::string& out_name) {
        expect(TokenKind::LParen, "'('");
        expect(TokenKind::Star, "'*'");
        ParsedAttributes ptr_attrs = parse_attribute_specifier_seq();
        out_name = std::string(expect(TokenKind::Identifier, "function pointer name").text);
        expect(TokenKind::RParen, "')'");
        Type type;
        type.kind = TypeKind::FunctionPointer;
        type.function_return = std::make_shared<Type>(std::move(return_type));
        type.function_params = parse_function_pointer_param_types();
        type.is_unsafe_function_pointer = ptr_attrs.has("unsafe");
        return type;
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
    void parse_module_declaration(Program& program) {
        bool leading_export = check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::KwModule;
        if (!leading_export && !check(TokenKind::KwModule)) return;
        if (leading_export) advance(); // 'export'
        advance(); // 'module'
        std::string dotted(expect(TokenKind::Identifier, "module name").text);
        while (match(TokenKind::Dot)) {
            dotted += '.';
            dotted += std::string(expect(TokenKind::Identifier, "module name segment").text);
        }
        // ch11 §11.4: an optional `:partition` suffix -- designates this
        // file as one specific partition of `dotted`, rather than its
        // primary interface/implementation unit. partition_name stays
        // separate from module_name (which always holds just the base
        // dotted name) so the export/namespace validation pass (§11.6)
        // keeps comparing against the *module's* own name, unaffected by
        // which partition happens to be declaring something.
        if (match(TokenKind::Colon)) {
            program.partition_name = std::string(expect(TokenKind::Identifier, "partition name").text);
        }
        expect(TokenKind::Semicolon, "';'");
        program.module_name = dotted;
        program.is_module_interface = leading_export;
        program.is_module_impl = !leading_export;
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
    void parse_import_declarations(Program& program) {
        for (;;) {
            bool is_reexport = check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::KwImport;
            bool is_plain_import = check(TokenKind::KwImport);
            if (!is_reexport && !is_plain_import) return;
            if (is_reexport) advance(); // 'export'
            const Token& import_tok = peek();
            advance(); // 'import'

            if (match(TokenKind::Colon)) {
                // ch11 §11.4: a same-module partition import -- only
                // meaningful inside a file that is itself part of some
                // module (primary unit or another partition).
                std::string partition_name(expect(TokenKind::Identifier, "partition name").text);
                expect(TokenKind::Semicolon, "';'");

                if (program.module_name.empty()) {
                    throw ParseError(import_tok.line, import_tok.column,
                                      "cannot import partition ':" + partition_name +
                                          "' -- this file has no 'module'/'export module' declaration of "
                                          "its own (ch11 §11.4: partitions only exist within a module)");
                }
                ImportDecl import;
                import.module_name = partition_name;
                import.is_reexport = is_reexport;
                import.is_partition = true;
                program.imports.push_back(import);

                std::string key = program.module_name + ":" + partition_name;
                if (!partition_resolver_) {
                    throw ParseError(import_tok.line, import_tok.column,
                                      "cannot resolve partition '" + key +
                                          "' -- no partition resolver was configured for this build (see "
                                          "the driver's --import " + key + "=path flag)");
                }
                merge_partition(program, partition_resolver_(key), import.is_reexport, key, import_tok);
                continue;
            }

            std::string dotted(expect(TokenKind::Identifier, "imported module name").text);
            while (match(TokenKind::Dot)) {
                dotted += '.';
                dotted += std::string(expect(TokenKind::Identifier, "module name segment").text);
            }
            expect(TokenKind::Semicolon, "';'");

            ImportDecl import;
            import.module_name = dotted;
            import.is_reexport = is_reexport;
            program.imports.push_back(std::move(import));

            if (!resolver_) {
                throw ParseError(import_tok.line, import_tok.column,
                                  "cannot resolve imported module '" + dotted +
                                      "' -- no module resolver was configured for this build (see the "
                                      "driver's --import name=path flag)");
            }
            merge_imported_module(program, resolver_(dotted), dotted, is_reexport);
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
        Function clone;
        clone.return_type = fn.return_type;
        clone.name = fn.name;
        clone.loc = fn.loc;
        clone.params = fn.params;
        if (keep_body && fn.body) clone.body = clone_stmt(*fn.body);
        clone.is_extern_c = fn.is_extern_c;
        clone.is_module_extern = fn.is_module_extern;
        clone.is_unsafe = fn.is_unsafe;
        clone.has_varargs = fn.has_varargs;
        clone.method_requires_concept = fn.method_requires_concept;
        clone.is_generic_template = fn.is_generic_template;
        clone.template_params = fn.template_params;
        clone.forwards_to = fn.forwards_to;
        clone.namespace_path = fn.namespace_path;
        clone.is_exported = is_reexport && fn.is_exported;
        clone.owning_module = fn.owning_module.empty() ? fallback_owning_module : fn.owning_module;
        return clone;
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
    void merge_imported_module(Program& program, const Program& imported, const std::string& imported_name,
                                bool is_reexport) {
        for (const StructDef& def : imported.structs) {
            if (!def.is_exported) continue;
            struct_names_.insert(def.name);
            if (!def.template_params.empty()) {
                generic_type_names_.insert(def.name);
                ordinary_generic_type_template_params_[def.name] = def.template_params;
            }
            StructDef clone = def;
            if (clone.owning_module.empty()) clone.owning_module = imported_name;
            clone.is_exported = is_reexport && clone.is_exported;
            program.structs.push_back(std::move(clone));
        }
        for (const ClassDef& def : imported.classes) {
            if (!def.is_exported) continue;
            struct_names_.insert(def.name);
            class_names_.insert(def.name);
            if (!def.template_params.empty() || def.is_variadic_primary_template) {
                generic_type_names_.insert(def.name);
                if (def.is_variadic_primary_template) {
                    variadic_primary_template_params_[def.name] = def.template_params;
                } else {
                    ordinary_generic_type_template_params_[def.name] = def.template_params;
                }
            }
            ClassDef clone = clone_class_def(def);
            if (clone.owning_module.empty()) clone.owning_module = imported_name;
            clone.is_exported = is_reexport && clone.is_exported;
            program.classes.push_back(std::move(clone));
        }
        for (const Function& fn : imported.functions) {
            if (!fn.is_exported) continue;
            if (!fn.template_params.empty()) generic_function_template_params_[fn.name] = fn.template_params;
            bool keep_body =
                fn.is_generic_template ||
                (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.pointee != nullptr &&
                 is_exported_generic_type_template(imported, fn.params[0].type.pointee->name));
            program.functions.push_back(clone_function_declaration(fn, imported_name, is_reexport, keep_body));
        }
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
    // `owning_module` is left empty on every merged declaration (they
    // become this Program's own local declarations, not a foreign
    // module's), which is also exactly why codegen's mangling (keyed off
    // owning_module, falling back to Program::module_name) still produces
    // the right module-qualified symbol for anything the whole merged
    // Program eventually exports.
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
    void merge_partition(Program& program, Program&& partition, bool is_reexport, const std::string& key,
                          const Token& import_tok) {
        if (is_reexport && partition.is_module_impl) {
            throw ParseError(import_tok.line, import_tok.column,
                              "cannot 'export import' partition '" + key +
                                  "': it is an implementation partition ('module ...;' with no 'export' on "
                                  "its own module declaration), so it can never export anything to the "
                                  "outside (ch11 §11.4)");
        }
        for (StructDef& def : partition.structs) {
            struct_names_.insert(def.name);
            if (!def.template_params.empty()) {
                generic_type_names_.insert(def.name);
                ordinary_generic_type_template_params_[def.name] = def.template_params;
            }
            def.is_exported = is_reexport && def.is_exported;
            def.owning_module.clear();
            program.structs.push_back(std::move(def));
        }
        for (ClassDef& def : partition.classes) {
            struct_names_.insert(def.name);
            class_names_.insert(def.name);
            if (!def.template_params.empty() || def.is_variadic_primary_template) {
                generic_type_names_.insert(def.name);
                if (def.is_variadic_primary_template) {
                    variadic_primary_template_params_[def.name] = def.template_params;
                } else {
                    ordinary_generic_type_template_params_[def.name] = def.template_params;
                }
            }
            def.is_exported = is_reexport && def.is_exported;
            def.owning_module.clear();
            program.classes.push_back(std::move(def));
        }
        for (Function& fn : partition.functions) {
            fn.is_exported = is_reexport && fn.is_exported;
            fn.owning_module.clear();
            program.functions.push_back(std::move(fn));
        }
    }

    // The main "loop over top-level declarations" body, shared between
    // file scope (`inside_namespace=false`, terminated only by
    // EndOfFile -- a stray '}' here is left unconsumed, surfacing as an
    // ordinary parse error downstream exactly as it always has) and the
    // inside of a `namespace { ... }` block (`inside_namespace=true`,
    // also terminated by the block's own closing '}').
    void parse_top_level_items(Program& program, bool inside_namespace = false) {
        while (!check(TokenKind::EndOfFile) && !(inside_namespace && check(TokenKind::RBrace))) {
            if (check(TokenKind::KwNamespace)) {
                parse_namespace_block(program);
                continue;
            }
            if (check(TokenKind::KwExport) && peek_at(1).kind == TokenKind::LBrace) {
                // `export { <item> <item> ... }` -- groups several
                // declarations under one export marker (ch11 §11.3),
                // equivalent to writing `export` before each
                // individually.
                advance(); // 'export'
                advance(); // '{'
                while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                    parse_top_level_item(program, /*is_exported=*/true);
                }
                expect(TokenKind::RBrace, "'}'");
                continue;
            }
            bool is_exported = match(TokenKind::KwExport);
            parse_top_level_item(program, is_exported);
        }
    }

    // Parses exactly one struct/class/concept/function(-or-extern-group)
    // top-level item, given whether an `export` marker (individual, or
    // inherited from an enclosing `export { }` group) applies to it.
    void parse_top_level_item(Program& program, bool is_exported) {
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
        const Token& attr_start_tok = peek();
        ParsedAttributes leading_attrs = parse_attribute_specifier_seq();
        bool requested_unsafe = leading_attrs.has("unsafe");
        auto reject_unsafe_if_requested = [&](const char* what) {
            if (requested_unsafe) {
                throw ParseError(attr_start_tok.line, attr_start_tok.column,
                                  "'[[scpp::unsafe]]' cannot appertain to " + std::string(what) +
                                      " -- only to a compound-statement or a function's own declaration "
                                      "(ch01 §1.3)");
            }
        };
        if (check(TokenKind::KwStruct)) {
            reject_unsafe_if_requested("a 'struct' declaration");
            SourceLocation loc = current_loc();
            StructDef def = parse_struct_def();
            def.is_exported = is_exported;
            check_export_namespace(program, is_exported, def.namespace_path, loc, "struct '" + def.name + "'");
            program.structs.push_back(std::move(def));
        } else if (check(TokenKind::KwClass)) {
            reject_unsafe_if_requested("a 'class' declaration");
            parse_class_def(program, is_exported);
        } else if (check(TokenKind::KwTemplate)) {
            // ch05 §5.11/§5.14: `template<...>` introduces either a
            // `concept` declaration or a generic `class`/`struct` type
            // -- peek past the whole header (of whatever length --
            // zero, one, or several parameters, possibly ending in a
            // pack) to see which of the three keywords follows, without
            // consuming anything yet.
            size_t after_header = offset_after_matching_angle(1); // peek_at(1) is the header's own '<'
            TokenKind after_header_kind = peek_at(after_header).kind;
            if (after_header_kind == TokenKind::KwClass) {
                reject_unsafe_if_requested("a 'class' declaration");
                // A class name is immediately followed by `;` (a
                // variadic primary template's own bodyless forward
                // declaration, e.g. `template<typename... Ts> class
                // Tuple;`), `<` (one of the two fixed specializations of
                // an already-declared primary template, e.g.
                // `template<> class Tuple<> { ... };`), or `{`/`:` (an
                // ordinary, single-template-parameter generic class,
                // ch05 §5.14's phase-1 shape).
                TokenKind after_name = peek_at(after_header + 2).kind;
                if (after_name == TokenKind::Semicolon) {
                    parse_variadic_primary_template_decl(program, is_exported);
                } else if (after_name == TokenKind::Less) {
                    parse_variadic_specialization(program, is_exported);
                } else {
                    std::vector<GenericTypeParam> template_params = parse_generic_type_header();
                    parse_class_def(program, is_exported, std::move(template_params));
                }
            } else if (after_header_kind == TokenKind::KwStruct) {
                reject_unsafe_if_requested("a 'struct' declaration");
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
                    throw ParseError(tok.line, tok.column,
                                      "a variadic generic type (parameter packs, ch05 §5.14) is only "
                                      "supported for 'class', never 'struct' -- building one needs recursive "
                                      "inheritance, which a struct doesn't have");
                }
                SourceLocation loc = current_loc();
                std::vector<GenericTypeParam> template_params = parse_generic_type_header();
                StructDef def = parse_struct_def(std::move(template_params));
                def.is_exported = is_exported;
                check_export_namespace(program, is_exported, def.namespace_path, loc, "struct '" + def.name + "'");
                program.structs.push_back(std::move(def));
            } else if (after_header_kind == TokenKind::KwConcept) {
                reject_unsafe_if_requested("a 'concept' declaration");
                parse_concept_def(program, is_exported);
            } else {
                // ch05 §5.11: neither `class`/`struct` (a generic type,
                // handled above) nor `concept` -- the only remaining
                // legal shape is a full-header-form generic *function*
                // (`template<...> ReturnType name(params) { body }`,
                // ch05 §5.11's "generic functions may be spelled with
                // either the abbreviated or full header form").
                parse_generic_function_def(program, is_exported, requested_unsafe);
            }
        } else {
            parse_top_level_function_or_extern_group(program, is_exported, requested_unsafe);
        }
    }

    // ch11 §11.4: `namespace a::b::c { ... }`, including the C++17
    // one-line nested form (all of `a::b::c` in one declaration) --
    // pushes every segment onto namespace_stack_, parses a nested
    // sequence of top-level items (recursively allowing further nested
    // namespace blocks), then pops them back off. No `export` may
    // precede `namespace` itself -- only individual declarations
    // *inside* it can be exported (ch11 §11.5); namespace and export are
    // independent axes, both required for an exported declaration to
    // actually export (see the export/namespace validation pass).
    void parse_namespace_block(Program& program) {
        expect(TokenKind::KwNamespace, "'namespace'");
        size_t pushed = 0;
        for (;;) {
            std::string segment(expect(TokenKind::Identifier, "namespace name").text);
            namespace_stack_.push_back(std::move(segment));
            pushed++;
            if (!match(TokenKind::ColonColon)) break;
        }
        expect(TokenKind::LBrace, "'{'");
        parse_top_level_items(program, /*inside_namespace=*/true);
        expect(TokenKind::RBrace, "'}'");
        for (size_t i = 0; i < pushed; i++) namespace_stack_.pop_back();
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
    void parse_top_level_function_or_extern_group(Program& program, bool is_exported, bool is_unsafe = false) {
        SourceLocation loc = current_loc();
        if (match(TokenKind::KwExtern)) {
            if (!check(TokenKind::StringLiteral)) {
                // Bare extern (ch11 §11.6): always a single bodyless
                // declaration, ordinary scpp linkage.
                Function fn = parse_function(/*is_extern_c=*/false, /*is_module_extern=*/true, is_unsafe);
                fn.loc = loc;
                fn.name = qualify_name(fn.name);
                fn.namespace_path = namespace_stack_;
                fn.is_exported = is_exported;
                check_export_namespace(program, is_exported, fn.namespace_path, loc, "function '" + fn.name + "'");
                program.functions.push_back(std::move(fn));
                return;
            }
            parse_c_linkage_string();
            if (check(TokenKind::LBrace)) {
                advance(); // '{'
                while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
                    if (check(TokenKind::KwStruct)) {
                        const Token& tok = peek();
                        throw ParseError(tok.line, tok.column,
                                          "an 'extern \"C\"' block currently only supports function "
                                          "declarations/definitions, not structs");
                    }
                    SourceLocation item_loc = current_loc();
                    Function item_fn = parse_function(/*is_extern_c=*/true);
                    item_fn.loc = item_loc;
                    program.functions.push_back(std::move(item_fn));
                }
                expect(TokenKind::RBrace, "'}'");
                return;
            }
            Function fn = parse_function(/*is_extern_c=*/true, /*is_module_extern=*/false, is_unsafe);
            fn.loc = loc;
            program.functions.push_back(std::move(fn));
            return;
        }
        Function fn = parse_function(/*is_extern_c=*/false, /*is_module_extern=*/false, is_unsafe);
        fn.loc = loc;
        fn.name = qualify_name(fn.name);
        fn.namespace_path = namespace_stack_;
        fn.is_exported = is_exported;
        check_export_namespace(program, is_exported, fn.namespace_path, loc, "function '" + fn.name + "'");
        program.functions.push_back(std::move(fn));
    }

    // Consumes and validates the linkage string literal after `extern`.
    // v0.1 only accepts the literal "C" (not "C++" or anything else) --
    // see ch02 §2.1.
    void parse_c_linkage_string() {
        const Token& tok = expect(TokenKind::StringLiteral, "a linkage string (e.g. \"C\")");
        // `tok.text` includes the surrounding quotes (see StringLiteral's
        // definition in lexer.cppm).
        if (tok.text != "\"C\"") {
            throw ParseError(tok.line, tok.column,
                              "unsupported linkage " + std::string(tok.text) +
                                  ": only extern \"C\" is supported in this version");
        }
    }

    // Decodes a StringLiteral token's text (e.g. "a\nb") into its byte
    // content. `tok.text` includes the surrounding double quotes (see
    // StringLiteral's definition in lexer.cppm). Supports the same
    // minimal named-escape set as decode_char_literal above: \n \t \r \\
    // \' \" \0 -- no hex/octal escapes. Unlike a char literal, any number
    // of characters (including zero -- an empty string "") is valid.
    std::string decode_string_literal(const Token& tok) {
        if (tok.text.size() < 2) {
            throw ParseError(tok.line, tok.column, "unterminated string literal " + std::string(tok.text));
        }
        std::string_view inner = tok.text.substr(1, tok.text.size() - 2);
        std::string result;
        result.reserve(inner.size());
        for (size_t i = 0; i < inner.size(); i++) {
            if (inner[i] != '\\') {
                result.push_back(inner[i]);
                continue;
            }
            if (i + 1 >= inner.size()) {
                throw ParseError(tok.line, tok.column,
                                  "invalid string literal " + std::string(tok.text) +
                                      ": trailing '\\' with no following escape character");
            }
            i++;
            switch (inner[i]) {
                case 'n': result.push_back('\n'); break;
                case 't': result.push_back('\t'); break;
                case 'r': result.push_back('\r'); break;
                case '0': result.push_back('\0'); break;
                case '\\': result.push_back('\\'); break;
                case '\'': result.push_back('\''); break;
                case '"': result.push_back('"'); break;
                default:
                    throw ParseError(tok.line, tok.column,
                                      "invalid string literal " + std::string(tok.text) +
                                          ": unsupported escape sequence '\\" + std::string(1, inner[i]) +
                                          "' (supported: \\n \\t \\r \\\\ \\' \\\" \\0)");
            }
        }
        return result;
    }

    // Decodes a CharLiteral token's text (e.g. 'a', '\n', '\\', '\'', '\0')
    // into its ordinal value. `tok.text` includes the surrounding single
    // quotes (see CharLiteral's definition in lexer.cppm). Supports the
    // same minimal named-escape set as decode_string_literal above: \n \t
    // \r \\ \' \" \0 -- no hex/octal escapes.
    long long decode_char_literal(const Token& tok) {
        // A well-formed literal is always at least `''` (2 quote chars);
        // anything shorter means the lexer hit EOF before a closing
        // quote (an unterminated literal) -- guard before the substr
        // below so that case reports a clear error instead of
        // underflowing `tok.text.size() - 2`.
        if (tok.text.size() < 2) {
            throw ParseError(tok.line, tok.column,
                              "unterminated char literal " + std::string(tok.text));
        }
        std::string_view inner = tok.text.substr(1, tok.text.size() - 2);
        if (inner.size() == 1 && inner[0] != '\\') {
            return static_cast<unsigned char>(inner[0]);
        }
        if (inner.size() == 2 && inner[0] == '\\') {
            switch (inner[1]) {
                case 'n': return '\n';
                case 't': return '\t';
                case 'r': return '\r';
                case '0': return '\0';
                case '\\': return '\\';
                case '\'': return '\'';
                case '"': return '"';
                default: break;
            }
        }
        throw ParseError(tok.line, tok.column,
                          "invalid char literal " + std::string(tok.text) +
                              ": must be exactly one character or one of the supported escape "
                              "sequences (\\n \\t \\r \\\\ \\' \\\" \\0)");
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
    StructDef parse_struct_def(std::vector<GenericTypeParam> template_params = {}) {
        expect(TokenKind::KwStruct, "'struct'");
        StructDef def;
        // ch05 §5.15: `struct [[scpp::thread_movable]] Name { ... };` --
        // real C++ grammar already gives a class-head an optional
        // attribute-specifier-seq right after its class-key, the same
        // slot `struct [[deprecated]] Name { ... };` would use.
        ParsedAttributes attrs = parse_attribute_specifier_seq();
        def.thread_movable_override = attrs.has("thread_movable");
        def.thread_shareable_override = attrs.has("thread_shareable");
        if (attrs.thread_movable_if_movable_expr || attrs.thread_movable_if_shareable_expr) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                             "'[[scpp::thread_movable_if(a, b)]]' is only supported on class declarations");
        }
        std::string bare_name = std::string(expect(TokenKind::Identifier, "struct name").text);
        def.name = qualify_name(bare_name);
        def.namespace_path = namespace_stack_;
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
                    throw ParseError(tok.line, tok.column,
                                      "a generic struct's own type parameter '" + param.name +
                                          "' cannot be bare -- struct field triviality (ch04 §4.1) is a "
                                          "whole-type property, so it must be constrained by a concept at the "
                                          "struct itself (ch05 §5.14): write 'template<Concept " + param.name +
                                          "> struct " + bare_name +
                                          "' instead, or use 'class' if per-method constraints are enough");
                }
                if (!param.is_non_type) {
                    struct_names_.insert(param.name);
                    class_names_.insert(param.name);
                }
            }
            if (saw_type && saw_non_type) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "ordinary generic structs cannot yet mix type and non-type template "
                                  "parameters in one parameter list");
            }
            generic_type_names_.insert(def.name);
            ordinary_generic_type_template_params_[def.name] = template_params;
        }
        def.template_params = template_params;

        expect(TokenKind::LBrace, "'{'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            StructField field;
            Type base = parse_type();
            if (starts_function_pointer_declarator()) {
                field.type = parse_function_pointer_declarator(std::move(base), field.name);
            } else {
                field.name = std::string(expect(TokenKind::Identifier, "field name").text);
                field.type = parse_array_suffix(base);
            }
            expect(TokenKind::Semicolon, "';'");
            def.fields.push_back(std::move(field));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Semicolon, "';'");

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

    // Parses a parenthesized, comma-separated parameter list `(<type>
    // <name>, ...)`, including the enclosing parens -- shared by every
    // class member function (method/constructor; a destructor is always
    // zero-arg, parsed directly). Deliberately separate from
    // parse_function's own inline version (which also handles extern
    // "C"'s trailing `...`, never relevant to a method/constructor): the
    // two are simple and small enough that duplicating this one loop
    // body is lower-risk than threading varargs-specific logic through a
    // shared helper.
    std::vector<Param> parse_param_list() {
        std::vector<Param> params;
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                Param param;
                Type base_type = parse_param_type(param.generic_concept);
                param.is_parameter_pack = match(TokenKind::Ellipsis);
                if (param.is_parameter_pack && param.generic_concept.empty() &&
                    !referenced_pack_type_param_name(base_type).has_value()) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "parameter packs are only supported for the abbreviated generic form "
                                      "('Concept auto&... args') in this version (ch05 §5.11)");
                }
                param.name = std::string(expect(TokenKind::Identifier, "parameter name").text);
                if (param.is_parameter_pack && !check(TokenKind::RParen)) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "a parameter pack must be the last parameter in the list (ch05 §5.11)");
                }
                // Same C-style declarator order (array suffix after the
                // name, decaying to pointer) as parse_function's own
                // parameter loop.
                Type param_type = parse_array_suffix(base_type);
                if (param_type.kind == TypeKind::Array) {
                    Type decayed;
                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                param.type = std::move(param_type);
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
                ParsedAttributes param_attrs = parse_attribute_specifier_seq();
                param.require_thread_movable = param_attrs.has("thread_movable");
                param.require_thread_shareable = param_attrs.has("thread_shareable");
                params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        return params;
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
        Param this_param;
        this_param.name = "this";
        Type this_type;
        this_type.kind = TypeKind::Reference;
        this_type.pointee = std::make_shared<Type>();
        this_type.pointee->kind = TypeKind::Named;
        this_type.pointee->name = class_name;
        this_type.is_mutable_ref = !is_const;
        this_param.type = std::move(this_type);
        return this_param;
    }

    // ch05 §5.11: generic (concept-constrained) *methods* aren't
    // supported in v0.1 -- only free functions are (matching every
    // example the spec itself demonstrates). Rather than silently
    // mismarking such a method (parse_param_list's "ConceptName auto"
    // syntax is shared with parse_function, so it would otherwise parse
    // without error, just with is_generic_template never set -- an
    // ordinary-looking Function that's actually unsound to compile as
    // one), reject it outright with a clear, scoped error message.
    void reject_generic_params(const std::vector<Param>& params, const std::string& what) {
        for (const Param& param : params) {
            if (!param.generic_concept.empty()) {
                throw ParseError(current_loc().line, current_loc().column,
                                  "a generic (concept-constrained) parameter is not supported on " + what +
                                      " in this version (ch05 §5.11 -- only a free function may be generic)");
            }
        }
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
    // to a concrete Type via `helper_param_types` right here, so nothing
    // about the parameter list itself needs to survive into
    // ConceptRequirement.
    ConceptRequirement parse_concept_requirement(const std::string& placeholder_name,
                                                   const std::unordered_map<std::string, Type>& helper_param_types) {
        ConceptRequirement req;
        bool is_compound = match(TokenKind::LBrace);

        const Token& receiver_tok = expect(TokenKind::Identifier, "the concept's own requires-parameter name");
        if (receiver_tok.text != placeholder_name) {
            throw ParseError(receiver_tok.line, receiver_tok.column,
                              "expected a requirement shaped as a call on '" + placeholder_name +
                                  "' (this concept's own constrained requires-parameter) -- v0.1 does not "
                                  "support an arbitrary requirement expression");
        }
        if (match(TokenKind::Dot)) {
            req.method_name = std::string(expect(TokenKind::Identifier, "method name").text);
        } else {
            req.method_name = "call";
        }
        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                const Token& arg_tok =
                    expect(TokenKind::Identifier, "a requirement argument (a requires-expression parameter name)");
                auto it = helper_param_types.find(std::string(arg_tok.text));
                if (it == helper_param_types.end()) {
                    throw ParseError(arg_tok.line, arg_tok.column,
                                      "'" + std::string(arg_tok.text) +
                                          "' is not one of this concept's own requires-expression parameters -- "
                                          "v0.1 only supports a requirement argument that is a bare reference to "
                                          "one of them");
                }
                req.arg_types.push_back(it->second);
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");

        if (is_compound) {
            expect(TokenKind::RBrace, "'}'");
            // ch05 §5.11: the result must be constrained to an *exact*
            // type -- 'std::same_as<T>' only, never
            // 'std::convertible_to<T>' (scpp has no implicit scalar
            // conversions at all, so the two would mean the same thing
            // anyway).
            expect(TokenKind::Arrow,
                   "'->' (a brace-wrapped requirement must be followed by a 'std::same_as<T>' constraint in "
                   "this version)");
            if (!check_std_qualified("same_as")) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "a compound requirement's constraint must be 'std::same_as<T>' in this "
                                  "version (never 'std::convertible_to<T>')");
            }
            consume_std_qualified();
            expect(TokenKind::Less, "'<'");
            req.return_type = parse_type();
            expect(TokenKind::Greater, "'>'");
            req.has_return_constraint = true;
        }
        expect(TokenKind::Semicolon, "';'");
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
    std::vector<GenericTypeParam> parse_generic_type_header() {
        expect(TokenKind::KwTemplate, "'template'");
        expect(TokenKind::Less, "'<'");
        std::vector<GenericTypeParam> params;
        if (!check(TokenKind::Greater)) {
            do {
                GenericTypeParam param;
                if (match(TokenKind::KwTypename)) {
                    param.is_pack = match(TokenKind::Ellipsis);
                    param.name = std::string(expect(TokenKind::Identifier, "template parameter name").text);
                } else if (check(TokenKind::KwInt) || check(TokenKind::KwBool) || check(TokenKind::KwChar)) {
                    // ch05 §5.14: a non-type parameter -- restricted to
                    // scalar types (only int/bool/char exist as scpp
                    // types so far; ptrdiff_t/fixed-width integers/
                    // float32_t/float64_t/size_t are all deferred until
                    // those types themselves exist).
                    param.is_non_type = true;
                    param.non_type_type = parse_unqualified_type();
                    param.name = std::string(expect(TokenKind::Identifier, "template parameter name").text);
                } else {
                    const Token& concept_tok = peek();
                    std::string concept_name(
                        expect(TokenKind::Identifier, "'typename', a scalar type, or a concept name").text);
                    if (!concept_names_.contains(concept_name)) {
                        throw ParseError(concept_tok.line, concept_tok.column,
                                          "'" + concept_name +
                                              "' is not a declared concept -- a generic type's template "
                                              "parameter must be introduced by 'typename', a scalar type, or "
                                              "an already-declared concept name (ch05 §5.14)");
                    }
                    param.concept_name = concept_name;
                    param.name = std::string(expect(TokenKind::Identifier, "template parameter name").text);
                }
                if (param.is_pack && !check(TokenKind::Greater)) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "a parameter pack ('typename... " + param.name +
                                          "') must be the last template parameter (ch05 §5.14)");
                }
                params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::Greater, "'>'");
        return params;
    }

    // ch05 §5.14: parses a generic method (or constructor)'s own,
    // optional `requires ConceptName<T>` clause -- real C++20 syntax
    // verbatim, appearing after the parameter list (and, for a method,
    // its trailing `const`) and before the body. `T` must name the
    // enclosing generic type's own single template parameter exactly
    // (this version has only one to match). Returns the concept's own
    // name (Function::method_requires_concept), or empty if no such
    // clause is present -- always empty when `template_params` itself
    // is empty (an ordinary, non-generic class/struct's member can never
    // have one, since there's no type parameter left to constrain).
    std::string parse_optional_method_requires_clause(const std::vector<GenericTypeParam>& template_params) {
        if (template_params.empty() || !check(TokenKind::KwRequires)) return "";
        advance(); // 'requires'
        std::string concept_name(expect(TokenKind::Identifier, "concept name").text);
        if (!concept_names_.contains(concept_name)) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "'" + concept_name + "' is not a declared concept (ch05 §5.14)");
        }
        expect(TokenKind::Less, "'<'");
        const Token& param_tok = peek();
        std::string arg_name(expect(TokenKind::Identifier, "the generic type's own template parameter name").text);
        if (arg_name != template_params[0].name) {
            throw ParseError(param_tok.line, param_tok.column,
                              "'requires " + concept_name + "<" + arg_name +
                                  ">' does not name this generic type's own template parameter '" +
                                  template_params[0].name + "' (ch05 §5.14)");
        }
        expect(TokenKind::Greater, "'>'");
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
    void parse_generic_function_def(Program& program, bool is_exported, bool is_unsafe = false) {
        SourceLocation loc = current_loc();
        std::vector<GenericTypeParam> template_params = parse_generic_type_header();
        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.insert(p.name);
            class_names_.insert(p.name);
        }

        std::vector<GenericTypeParam> saved_template_params = current_function_template_params_;
        current_function_template_params_ = template_params;
        Function fn = parse_function(/*is_extern_c=*/false, /*is_module_extern=*/false, is_unsafe);
        current_function_template_params_ = std::move(saved_template_params);
        fn.loc = loc;
        fn.name = qualify_name(fn.name);
        fn.namespace_path = namespace_stack_;
        fn.is_exported = is_exported;
        fn.template_params = template_params;
        fn.is_generic_template = true;
        check_export_namespace(program, is_exported, fn.namespace_path, loc, "function '" + fn.name + "'");
        generic_function_template_params_[fn.name] = template_params;

        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.erase(p.name);
            class_names_.erase(p.name);
        }
        program.functions.push_back(std::move(fn));
    }

    void parse_concept_def(Program& program, bool is_exported) {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwTemplate, "'template'");
        expect(TokenKind::Less, "'<'");
        expect(TokenKind::KwTypename, "'typename'");
        std::string template_param_name =
            std::string(expect(TokenKind::Identifier, "template parameter name").text);
        expect(TokenKind::Greater, "'>'");

        expect(TokenKind::KwConcept, "'concept'");
        ConceptDef def;
        std::string bare_name = std::string(expect(TokenKind::Identifier, "concept name").text);
        def.name = qualify_name(bare_name);
        def.template_param_name = template_param_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        check_export_namespace(program, is_exported, def.namespace_path, loc, "concept '" + def.name + "'");
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

        expect(TokenKind::Assign, "'='");
        expect(TokenKind::KwRequires, "'requires'");
        expect(TokenKind::LParen, "'('");

        // The requires-expression's own (fake, unevaluated) parameter
        // list: exactly one parameter's declared type must be
        // (optionally const-qualified) the template parameter itself --
        // e.g. `const T& t` -- identifying it as the constrained
        // placeholder (def.requires_param_name), with its const-ness
        // driving the witness methods' own `this` mutability (mirroring
        // make_this_param). Every other parameter is an ordinary,
        // already-declared concrete type (e.g. `int x`), tracked only
        // transiently to resolve a requirement's own argument types.
        std::unordered_map<std::string, Type> helper_param_types;
        bool found_placeholder = false;
        bool placeholder_is_const = false;
        if (!check(TokenKind::RParen)) {
            do {
                size_t const_offset = check(TokenKind::KwConst) ? 1 : 0;
                bool is_placeholder = peek_at(const_offset).kind == TokenKind::Identifier &&
                                       peek_at(const_offset).text == template_param_name;
                if (is_placeholder) {
                    if (found_placeholder) {
                        const Token& tok = peek();
                        throw ParseError(tok.line, tok.column,
                                          "a concept's requires-expression may only have one parameter of "
                                          "the constrained type '" +
                                              template_param_name + "'");
                    }
                    placeholder_is_const = match(TokenKind::KwConst);
                    advance(); // the template parameter name itself (e.g. "T")
                    match(TokenKind::Amp); // optional trailing '&' -- ref-ness itself
                                            // doesn't affect witness synthesis, only
                                            // const-ness does
                    def.requires_param_name =
                        std::string(expect(TokenKind::Identifier, "requires-parameter name").text);
                    found_placeholder = true;
                } else {
                    Type helper_type = parse_type();
                    std::string helper_name =
                        std::string(expect(TokenKind::Identifier, "requires-parameter name").text);
                    helper_param_types[helper_name] = std::move(helper_type);
                }
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        if (!found_placeholder) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "concept '" + def.name +
                                  "'s requires-expression must have exactly one parameter of the "
                                  "constrained type '" +
                                  template_param_name + "'");
        }

        expect(TokenKind::LBrace, "'{'");
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            def.requirements.push_back(parse_concept_requirement(def.requires_param_name, helper_param_types));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Semicolon, "';'");

        ClassDef witness;
        witness.name = def.name;
        witness.namespace_path = namespace_stack_;
        witness.is_exported = is_exported;
        witness.is_concept_witness = true;
        program.classes.push_back(std::move(witness));

        for (const ConceptRequirement& req : def.requirements) {
            Function fn;
            fn.loc = loc;
            fn.name = def.name + "_" + req.method_name;
            fn.namespace_path = namespace_stack_;
            fn.is_exported = is_exported;
            fn.return_type =
                req.has_return_constraint ? req.return_type : Type{.kind = TypeKind::Named, .name = "void"};
            fn.params.push_back(make_this_param(def.name, placeholder_is_const));
            for (size_t i = 0; i < req.arg_types.size(); i++) {
                Param p;
                p.type = req.arg_types[i];
                p.name = "arg" + std::to_string(i);
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
    void parse_variadic_primary_template_decl(Program& program, bool is_exported) {
        SourceLocation loc = current_loc();
        std::vector<GenericTypeParam> template_params = parse_generic_type_header();
        expect(TokenKind::KwClass, "'class'");
        std::string class_name = std::string(expect(TokenKind::Identifier, "class name").text);
        std::string qualified_class_name = qualify_name(class_name);
        expect(TokenKind::Semicolon, "';'");

        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);
        generic_type_names_.insert(qualified_class_name);
        variadic_primary_template_params_[qualified_class_name] = template_params;

        ClassDef def;
        def.name = qualified_class_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = template_params;
        def.is_variadic_primary_template = true;
        check_export_namespace(program, is_exported, def.namespace_path, loc, "class '" + qualified_class_name + "'");
        program.classes.push_back(std::move(def));
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
    void parse_variadic_specialization(Program& program, bool is_exported) {
        SourceLocation loc = current_loc();
        std::vector<GenericTypeParam> template_params = parse_generic_type_header();
        expect(TokenKind::KwClass, "'class'");
        const Token& class_name_tok = peek();
        std::string class_name = std::string(expect(TokenKind::Identifier, "class name").text);
        std::string qualified_class_name = qualify_name(class_name);
        auto primary_it = variadic_primary_template_params_.find(qualified_class_name);
        if (primary_it == variadic_primary_template_params_.end()) {
            throw ParseError(class_name_tok.line, class_name_tok.column,
                              "'" + qualified_class_name +
                                  "' is not a declared variadic primary template -- a specialization requires "
                                  "a preceding 'template<typename... Ts> class " +
                                  class_name + ";' forward declaration (ch05 §5.14)");
        }

        // The specialization's own `<...>` argument list must exactly
        // restate template_params' own names, in order (empty for the
        // base case).
        expect(TokenKind::Less, "'<'");
        size_t index = 0;
        if (!check(TokenKind::Greater)) {
            do {
                if (index >= template_params.size()) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "this specialization's own '<...>' argument list has more entries than "
                                      "its own template header (ch05 §5.14 only supports restating the "
                                      "header's own parameter names, in order)");
                }
                const Token& name_tok = peek();
                std::string arg_name = std::string(expect(TokenKind::Identifier, "template parameter name").text);
                if (arg_name != template_params[index].name) {
                    throw ParseError(name_tok.line, name_tok.column,
                                      "expected this specialization's own template parameter '" +
                                          template_params[index].name + "', not '" + arg_name +
                                          "' (ch05 §5.14 only supports restating the header's own parameter "
                                          "names, in order)");
                }
                if (template_params[index].is_pack) expect(TokenKind::Ellipsis, "'...'");
                index++;
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::Greater, "'>'");
        if (index != template_params.size()) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "this specialization's own '<...>' argument list must restate every one of its "
                              "own template header's parameters (ch05 §5.14)");
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
        size_t leading_non_type_count = 0;
        while (leading_non_type_count < template_params.size() &&
               template_params[leading_non_type_count].is_non_type) {
            leading_non_type_count++;
        }
        for (size_t i = leading_non_type_count; i < template_params.size(); i++) {
            if (template_params[i].is_non_type) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "a variadic specialization's non-type parameter(s) must all come first, "
                                  "before any type parameter (ch05 §5.14)");
            }
        }
        size_t remaining = template_params.size() - leading_non_type_count;
        bool has_pack = remaining == 2 && template_params[leading_non_type_count + 1].is_pack &&
                         !template_params[leading_non_type_count].is_pack;
        if (remaining != 0 && !has_pack) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "a variadic specialization's own template header must, after any leading "
                              "non-type parameter(s), be either empty (the empty-pack base case) or end in "
                              "exactly one type parameter followed by a parameter pack ('typename Head, "
                              "typename... Tail', the recursive case) (ch05 §5.14)");
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

        ClassDef def;
        def.name = qualified_class_name;
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = template_params;
        def.is_variadic_specialization = true;
        check_export_namespace(program, is_exported, def.namespace_path, loc, "class '" + qualified_class_name + "'");

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
        // ClassDef::base_pack_arg_name's own comment).
        if (match(TokenKind::Colon)) {
            if (match(TokenKind::KwPublic)) {
                def.base_access = AccessSpecifier::Public;
            } else {
                match(TokenKind::KwPrivate);
                def.base_access = AccessSpecifier::Private;
            }
            const Token& base_tok = peek();
            std::string base_name = parse_qualified_name();
            auto base_primary_it = variadic_primary_template_params_.find(base_name);
            if (base_primary_it == variadic_primary_template_params_.end()) {
                throw ParseError(base_tok.line, base_tok.column,
                                  "'" + base_name + "' is not a declared variadic primary template (ch05 §5.14)");
            }
            def.base_class_name = base_name;
            size_t base_leading_non_type_count = 0;
            for (const GenericTypeParam& p : base_primary_it->second) {
                if (!p.is_non_type) break;
                base_leading_non_type_count++;
            }
            expect(TokenKind::Less, "'<'");
            for (size_t i = 0; i < base_leading_non_type_count; i++) {
                def.base_non_type_arg = std::shared_ptr<Expr>(parse_additive().release());
                expect(TokenKind::Comma, "','");
            }
            const Token& pack_tok = peek();
            std::string pack_name = std::string(expect(TokenKind::Identifier, "the pack parameter's own name").text);
            if (!has_pack || pack_name != template_params.back().name) {
                throw ParseError(pack_tok.line, pack_tok.column,
                                  "a variadic specialization's own base class can only be instantiated by "
                                  "spreading this specialization's own pack parameter whole (e.g. '" +
                                      base_name + "<" + (has_pack ? template_params.back().name : "Tail") +
                                      "...>') (ch05 §5.14)");
            }
            expect(TokenKind::Ellipsis, "'...'");
            expect(TokenKind::Greater, "'>'");
            def.base_pack_arg_name = pack_name;
        }

        parse_class_body_into(program, def, class_name, template_params);

        for (const GenericTypeParam& p : template_params) {
            if (p.is_non_type) continue;
            struct_names_.erase(p.name);
            class_names_.erase(p.name);
        }
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
    void parse_class_def(Program& program, bool is_exported, std::vector<GenericTypeParam> template_params = {}) {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwClass, "'class'");
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
        ParsedAttributes class_attrs = parse_attribute_specifier_seq();
        // The bare, unqualified name as written -- used for the
        // constructor/destructor spelling checks below (`~string()`,
        // `string(...)`  inside the class body itself always use the
        // bare name, exactly like real C++, never a namespace-qualified
        // one). `qualified_class_name` (namespace_stack_-prefixed, ch11
        // §11.4) is the form used everywhere this class is *referred to*
        // from outside its own body: struct_names_/class_names_
        // registration, `this`'s declared type, and every synthesized
        // member function's own name.
        std::string class_name = std::string(expect(TokenKind::Identifier, "class name").text);
        std::string qualified_class_name = qualify_name(class_name);
        // Register the name before parsing the body so a field/method can
        // refer to the enclosing class via a pointer, and so a
        // self-referential constructor call/access-control decision below
        // already recognizes it -- same before-parsing-the-body
        // registration order as parse_struct_def.
        struct_names_.insert(qualified_class_name);
        class_names_.insert(qualified_class_name);
        if (is_generic) {
            bool saw_type = false;
            bool saw_non_type = false;
            for (const GenericTypeParam& param : template_params) {
                saw_type = saw_type || !param.is_non_type;
                saw_non_type = saw_non_type || param.is_non_type;
            }
            if (saw_type && saw_non_type) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "ordinary generic classes cannot yet mix type and non-type template "
                                  "parameters in one parameter list");
            }
            generic_type_names_.insert(qualified_class_name);
            ordinary_generic_type_template_params_[qualified_class_name] = template_params;
        }

        ClassDef def;
        def.name = qualified_class_name;
        def.thread_movable_override = class_attrs.has("thread_movable");
        def.thread_shareable_override = class_attrs.has("thread_shareable");
        if (class_attrs.thread_movable_if_movable_expr || class_attrs.thread_movable_if_shareable_expr) {
            if (!(class_attrs.thread_movable_if_movable_expr && class_attrs.thread_movable_if_shareable_expr)) {
                throw ParseError(loc.line, loc.column,
                                 "'[[scpp::thread_movable_if(a, b)]]' requires exactly two boolean arguments");
            }
            if (def.thread_movable_override || def.thread_shareable_override) {
                throw ParseError(loc.line, loc.column,
                                 "'[[scpp::thread_movable_if(a, b)]]' cannot be combined with bare "
                                 "'[[scpp::thread_movable]]' or '[[scpp::thread_shareable]]' on the same class");
            }
            def.thread_movable_if_movable_expr = std::move(class_attrs.thread_movable_if_movable_expr);
            def.thread_movable_if_shareable_expr = std::move(class_attrs.thread_movable_if_shareable_expr);
        }
        def.namespace_path = namespace_stack_;
        def.is_exported = is_exported;
        def.template_params = template_params;
        check_export_namespace(program, is_exported, def.namespace_path, loc, "class '" + qualified_class_name + "'");

        // ch05 §5.14: `class Derived : public/private Base { ... };` --
        // real C++ single-inheritance syntax verbatim. `Base` must
        // already be a declared class (this parser is single-pass, same
        // requirement as every other type reference) -- a generic
        // type's own base (e.g. `Tuple<Head, Tail...> : private
        // Tuple<Tail...>`) is handled separately by the specialization
        // parser, which never reaches this ordinary path.
        if (match(TokenKind::Colon)) {
            // Real C++'s own default when neither keyword is written is
            // `private` for a `class` (only `struct` defaults to
            // `public`, but structs have no inheritance here at all) --
            // matching that exactly rather than requiring the keyword.
            if (match(TokenKind::KwPublic)) {
                def.base_access = AccessSpecifier::Public;
            } else {
                match(TokenKind::KwPrivate); // optional -- private either way
                def.base_access = AccessSpecifier::Private;
            }
            const Token& base_tok = peek();
            std::string base_name = parse_qualified_name();
            if (!class_names_.contains(base_name)) {
                throw ParseError(base_tok.line, base_tok.column,
                                  "'" + base_name +
                                      "' is not a declared class -- a base class must be declared before use "
                                      "(ch05 §5.14), and only a class (never a struct, ch04 §4.1) may be one");
            }
            def.base_class_name = base_name;
        }

        parse_class_body_into(program, def, class_name, template_params);

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
    // base_class_name/base_access/is_variadic_specialization must
    // already be set by the caller; only `fields` is populated here.
    void parse_class_body_into(Program& program, ClassDef& def, const std::string& class_name,
                                const std::vector<GenericTypeParam>& template_params) {
        std::string qualified_class_name = def.name;
        bool is_exported = def.is_exported;
        // Every method/constructor/destructor synthesized below shares
        // this same namespace_path/is_exported (ch11 §11.3: exporting a
        // class exports its whole member surface as one unit) --
        // owning_module stays default-empty (this program's own
        // declaration; only set later, at cross-module merge time).
        auto finish_member_fn = [&](Function& fn) {
            fn.namespace_path = namespace_stack_;
            fn.is_exported = is_exported;
        };
        auto enter_member_template_context = [&](const std::vector<GenericTypeParam>& member_template_params) {
            for (const GenericTypeParam& p : member_template_params) {
                if (p.is_non_type) continue;
                struct_names_.insert(p.name);
                class_names_.insert(p.name);
            }
            current_function_template_params_ = member_template_params;
        };
        auto leave_member_template_context = [&](const std::vector<GenericTypeParam>& member_template_params,
                                                 std::vector<GenericTypeParam>& saved_function_template_params) {
            current_function_template_params_ = std::move(saved_function_template_params);
            for (const GenericTypeParam& p : member_template_params) {
                if (p.is_non_type) continue;
                struct_names_.erase(p.name);
                class_names_.erase(p.name);
            }
        };
        std::vector<GenericTypeParam> saved_class_template_params = current_class_template_params_;
        current_class_template_params_ = template_params;

        expect(TokenKind::LBrace, "'{'");
        // Real C++'s own default: a class body starts `private` until the
        // first access-specifier section (unlike `struct`, which has no
        // access control at all -- ch04 §4.2).
        AccessSpecifier current_access = AccessSpecifier::Private;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::KwPublic)) {
                expect(TokenKind::Colon, "':'");
                current_access = AccessSpecifier::Public;
                continue;
            }
            if (match(TokenKind::KwPrivate)) {
                expect(TokenKind::Colon, "':'");
                current_access = AccessSpecifier::Private;
                continue;
            }

            SourceLocation member_loc = current_loc();
            bool member_is_template = false;
            std::vector<GenericTypeParam> member_template_params;
            std::vector<GenericTypeParam> saved_function_template_params = current_function_template_params_;
            if (check(TokenKind::KwTemplate)) {
                member_template_params = parse_generic_type_header();
                member_is_template = true;
                enter_member_template_context(member_template_params);
            }
            // ch01 §1.2/§1.3: `[[scpp::unsafe]]` on a method/constructor/
            // destructor's own declaration -- identical function-level
            // marker semantics as a free function (see parse_function's
            // own handling), just reached from inside a class body
            // instead. Parsed once here, before dispatching on which
            // member shape follows; rejected if it turns out to be a
            // field (unsafe is only ever meaningful on a function, ch01
            // §1.3 (1)).
            const Token& member_attr_start_tok = peek();
            ParsedAttributes member_attrs = parse_attribute_specifier_seq();
            bool member_requested_unsafe = member_attrs.has("unsafe");
            if (match(TokenKind::Tilde)) {
                if (member_is_template) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "a destructor cannot be a member template");
                }
                // Destructor: `~ClassName() { ... }` -- no parameters, no
                // return type, and always a mutable (non-`const`) `this`
                // (it always needs to tear the receiver down).
                const Token& name_tok = expect(TokenKind::Identifier, "destructor name");
                if (std::string(name_tok.text) != class_name) {
                    throw ParseError(name_tok.line, name_tok.column,
                                      "destructor name '~" + std::string(name_tok.text) +
                                          "' must match the enclosing class name '" + class_name + "'");
                }
                expect(TokenKind::LParen, "'('");
                expect(TokenKind::RParen, "')'");
                Function fn;
                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = qualified_class_name + "_delete";
                fn.params.push_back(make_this_param(qualified_class_name, /*is_const=*/false));
                fn.body = parse_block();
                finish_member_fn(fn);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }

            if (check(TokenKind::Identifier) && std::string(peek().text) == class_name &&
                peek_at(1).kind == TokenKind::LParen) {
                // Constructor: `ClassName(args) { ... }` -- distinguished
                // from an ordinary method/field by its name matching the
                // class exactly with *no* declared return type at all
                // (real C++'s own rule: the constructor is the one member
                // that never has a return type, not even `void`) --
                // `this` is always mutable here too (it's what the
                // constructor initializes).
                advance(); // class name
                Function fn;
                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.return_type.kind = TypeKind::Named;
                fn.return_type.name = "void";
                fn.name = qualified_class_name + "_new";
                fn.params = parse_param_list();
                reject_generic_params(fn.params, "a constructor");
                fn.template_params = member_template_params;
                fn.is_generic_template = member_is_template;
                // spec §6.4(1): a program shall not declare a move
                // constructor for a class type -- exactly one parameter,
                // of type rvalue reference to the class's own type (real
                // C++'s own [class.copy.ctor] classification). Every
                // class instead gets a compiler-synthesized one (spec
                // §6.4(2)), dispatched directly at each `ClassName y
                // (std::move(x));` call site (see movecheck's
                // is_move_construction_shape) -- never through this
                // user-declared-constructor machinery at all. See the
                // `operator=` parsing block further below for spec
                // §6.4(1)'s other half (a move *assignment* operator,
                // `ClassName& operator=(ClassName&& other)`), rejected by
                // the identical shape check.
                if (fn.params.size() == 1 && fn.params[0].type.kind == TypeKind::Reference &&
                    fn.params[0].type.is_rvalue_ref && fn.params[0].type.pointee &&
                    fn.params[0].type.pointee->kind == TypeKind::Named &&
                    fn.params[0].type.pointee->name == class_name) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "a move constructor cannot be user-declared for class '" + class_name +
                                          "' -- the compiler always provides one (spec §6.4(1)/(2), ch04 §4.2)");
                }
                fn.params.insert(fn.params.begin(), make_this_param(qualified_class_name, /*is_const=*/false));
                fn.method_requires_concept = parse_optional_method_requires_clause(template_params);
                fn.body = parse_block();
                finish_member_fn(fn);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }

            // Otherwise: an ordinary field, method, `operator*`, or `operator=` --
            // all start with a declared type -- same "parse a type,
            // then a name, then see what follows" disambiguation
            // parse_var_decl/parse_struct_def already use.
            Type member_type = parse_type();
            // spec §6.5: `ReturnType operator=(Params) { ... }` -- a
            // user-declared copy (or otherwise custom) assignment
            // operator. Unlike a move constructor (spec §6.4(1), always
            // rejected), a program *may* freely declare its own -- no
            // shape restriction here at all, mirroring how an ordinary
            // constructor accepts any parameter list; only the exact
            // shape `operator=(const ClassName&)` is later recognized
            // (movecheck's has_user_declared_copy_assign) as *the* copy
            // assignment operator for spec §6.5(3)'s auto-generation-
            // suppression rule -- any other parameter shape is simply an
            // ordinary (if unusual) overload of the name, resolved like
            // any other method. Distinguished from an ordinary method
            // named "operator" (impossible: `operator` is not a
            // keyword, but the very next token being `=` -- itself
            // never a legal start of a parameter list -- makes this
            // unambiguous) by peeking one token ahead before committing.
            if (check(TokenKind::Identifier) && std::string(peek().text) == "operator" &&
                peek_at(1).kind == TokenKind::Star) {
                if (member_is_template) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "an operator* cannot currently be a member template");
                }
                advance(); // 'operator'
                advance(); // '*'
                Function fn;
                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.params = parse_param_list();
                reject_generic_params(fn.params, "an operator*");
                bool is_const = match(TokenKind::KwConst);
                fn.return_type = std::move(member_type);
                fn.name = qualified_class_name + "_operator_deref";
                fn.params.insert(fn.params.begin(), make_this_param(qualified_class_name, is_const));
                fn.body = parse_block();
                finish_member_fn(fn);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (check(TokenKind::Identifier) && std::string(peek().text) == "operator" &&
                peek_at(1).kind == TokenKind::Assign) {
                if (member_is_template) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "an operator= cannot currently be a member template");
                }
                advance(); // 'operator'
                advance(); // '='
                Function fn;
                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.params = parse_param_list();
                reject_generic_params(fn.params, "an operator=");
                // spec §6.4(1): a program shall not declare a move
                // assignment operator for a class type either -- the
                // identical shape check the constructor case above uses
                // for a move *constructor* (exactly one parameter, of
                // type rvalue reference to the class's own type). Every
                // class instead gets a compiler-synthesized one
                // (dispatched directly at each `y = std::move(x);` call
                // site, see movecheck's is_move_construction_shape) --
                // never through this user-declared-operator machinery.
                // This was a real, discovered-and-fixed gap: unlike the
                // constructor case, this shape check didn't exist at all
                // when `operator=` parsing was first added, so a
                // user-declared move assignment operator silently parsed
                // as an ordinary (if unusual) overload instead of being
                // rejected.
                if (fn.params.size() == 1 && fn.params[0].type.kind == TypeKind::Reference &&
                    fn.params[0].type.is_rvalue_ref && fn.params[0].type.pointee &&
                    fn.params[0].type.pointee->kind == TypeKind::Named &&
                    fn.params[0].type.pointee->name == class_name) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "a move assignment operator cannot be user-declared for class '" +
                                          class_name + "' -- the compiler always provides one (spec "
                                          "§6.4(1)/(2), ch04 §4.2)");
                }
                bool is_const = match(TokenKind::KwConst);
                fn.return_type = std::move(member_type);
                fn.name = qualified_class_name + "_operator_assign";
                fn.params.insert(fn.params.begin(), make_this_param(qualified_class_name, is_const));
                fn.body = parse_block();
                finish_member_fn(fn);
                program.functions.push_back(std::move(fn));
                continue;
            }
            if (starts_function_pointer_declarator()) {
                if (member_is_template) {
                    throw ParseError(member_loc.line, member_loc.column,
                                      "a member template declaration must declare a constructor or method, not a field");
                }
                if (member_requested_unsafe) {
                    throw ParseError(member_attr_start_tok.line, member_attr_start_tok.column,
                                      "'[[scpp::unsafe]]' cannot appertain to a member variable -- only to a "
                                      "compound-statement or a function's own declaration (ch01 §1.3)");
                }
                ClassField field;
                field.type = parse_function_pointer_declarator(std::move(member_type), field.name);
                field.access = current_access;
                expect(TokenKind::Semicolon, "';'");
                def.fields.push_back(std::move(field));
                continue;
            }
            std::string member_name = std::string(expect(TokenKind::Identifier, "field or method name").text);
            if (check(TokenKind::LParen)) {
                Function fn;
                fn.loc = member_loc;
                fn.is_unsafe = member_requested_unsafe;
                fn.params = parse_param_list();
                reject_generic_params(fn.params, "a method");
                fn.template_params = member_template_params;
                fn.is_generic_template = member_is_template;
                // `const` trails the parameter list, exactly like real
                // C++ (`int length() const { ... }`), so it's only
                // knowable -- and `this`'s mutability with it -- after
                // parsing the params above.
                bool is_const = match(TokenKind::KwConst);
                fn.return_type = std::move(member_type);
                fn.name = qualified_class_name + "_" + member_name;
                fn.params.insert(fn.params.begin(), make_this_param(qualified_class_name, is_const));
                fn.method_requires_concept = parse_optional_method_requires_clause(template_params);
                fn.body = parse_block();
                finish_member_fn(fn);
                program.functions.push_back(std::move(fn));
                if (member_is_template) leave_member_template_context(member_template_params, saved_function_template_params);
                continue;
            }

            // ch04 §4.2: a field may now be public or private in any
            // combination, exactly like real C++ (the class-only
            // "member variables can never be public" restriction was
            // reversed -- see the doc's own commit history). Direct
            // external access to a public field is checked exactly like
            // a struct field access (movecheck's apply_expr Member
            // case: the whole-root-conservative treatment, not a new
            // per-field mechanism).
            if (member_requested_unsafe) {
                throw ParseError(member_attr_start_tok.line, member_attr_start_tok.column,
                                  "'[[scpp::unsafe]]' cannot appertain to a member variable -- only to a "
                                  "compound-statement or a function's own declaration (ch01 §1.3)");
            }
            if (member_is_template) {
                throw ParseError(member_loc.line, member_loc.column,
                                  "a member template declaration must declare a constructor or method, not a field");
            }
            ClassField field;
            field.type = parse_array_suffix(member_type);
            field.name = member_name;
            field.access = current_access;
            expect(TokenKind::Semicolon, "';'");
            def.fields.push_back(std::move(field));
        }
        expect(TokenKind::RBrace, "'}'");
        expect(TokenKind::Semicolon, "';'");
        current_class_template_params_ = std::move(saved_class_template_params);
        program.classes.push_back(std::move(def));
    }

    // Parses one function declaration or definition's `<return-type>
    // <name>(<params>)` followed by either `;` (a bodyless declaration --
    // legal when `is_extern_c` (ch02 §2.1) or `is_module_extern` (bare
    // `extern`, ch11 §11.6)) or `{ <body> }` (an ordinary definition).
    // `is_extern_c`/`is_module_extern` are decided and consumed by the
    // caller (parse_top_level_function_or_extern_group) before this
    // runs, since their combination affects *which* prefixes were
    // already consumed (an item inside an `extern "C" { }` block isn't
    // preceded by its own `extern "C"` -- see that function).
    Function parse_function(bool is_extern_c, bool is_module_extern = false, bool is_unsafe = false) {
        Function fn;
        fn.is_extern_c = is_extern_c;
        fn.is_module_extern = is_module_extern;
        fn.is_unsafe = is_unsafe;
        fn.return_type = parse_type();
        fn.name = std::string(expect(TokenKind::Identifier, "function name").text);

        expect(TokenKind::LParen, "'('");
        if (!check(TokenKind::RParen)) {
            do {
                if (match(TokenKind::Ellipsis)) {
                    // `...` must be the last thing in the parameter list
                    // (as in real C++) -- anything after it is left
                    // unconsumed, so the expect(RParen) below reports a
                    // clear parse error for e.g. `(..., int x)`.
                    fn.has_varargs = true;
                    break;
                }
                Param param;
                Type base_type = parse_param_type(param.generic_concept);
                param.is_parameter_pack = match(TokenKind::Ellipsis);
                if (param.is_parameter_pack && param.generic_concept.empty() &&
                    !referenced_pack_type_param_name(base_type).has_value()) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "parameter packs are only supported for the abbreviated generic form "
                                      "('Concept auto&... args') in this version (ch05 §5.11)");
                }
                param.name = std::string(expect(TokenKind::Identifier, "parameter name").text);
                if (param.is_parameter_pack && !check(TokenKind::RParen)) {
                    const Token& tok = peek();
                    throw ParseError(tok.line, tok.column,
                                      "a parameter pack must be the last parameter in the list (ch05 §5.11)");
                }
                // The array suffix (if any) follows the *declared name*,
                // not the type -- same C-style declarator order as
                // parse_var_decl/parse_struct_def (e.g. `int arr[4]`).
                Type param_type = parse_array_suffix(base_type);
                if (param_type.kind == TypeKind::Array) {
                    // A fixed-size array parameter decays to a pointer to
                    // its element type, exactly as in ordinary C++ (ch02
                    // §2.1's signature-type rules explicitly require this
                    // for `extern "C"`, and there's no reason for it to
                    // behave differently for an ordinary function): the
                    // array's *size* isn't part of the decayed type, only
                    // its element type is -- `int arr[4]` and `int* arr`
                    // are the same parameter type.
                    Type decayed;
                    decayed.kind = TypeKind::Pointer;
                    decayed.pointee = param_type.element;
                    param_type = std::move(decayed);
                }
                param.type = std::move(param_type);
                // ch05 §5.15: see parse_param_list's identical trailing-
                // attribute handling -- this is the separate parameter-
                // parsing loop parse_function itself uses (top-level
                // ordinary/generic/extern functions), not shared with
                // parse_param_list (class methods/lambdas).
                ParsedAttributes param_attrs = parse_attribute_specifier_seq();
                param.require_thread_movable = param_attrs.has("thread_movable");
                param.require_thread_shareable = param_attrs.has("thread_shareable");
                fn.params.push_back(std::move(param));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");

        // ch05 §5.11: a function with at least one concept-constrained
        // parameter is a *generic template* -- checked once, abstractly,
        // against each constrained parameter's own witness class; never
        // emitted to codegen directly (see Function::is_generic_template's
        // own comment). Computed here (rather than by scanning
        // program.functions later) since this is the one place every
        // function's parameter list is fully parsed, regardless of which
        // top-level path reached it.
        for (const Param& param : fn.params) {
            if (!param.generic_concept.empty()) {
                fn.is_generic_template = true;
                break;
            }
        }

        if (fn.has_varargs && !fn.is_extern_c) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "variadic parameters ('...') are only supported in an 'extern \"C\"' "
                              "declaration (ch02 §2.1)");
        }

        if (match(TokenKind::Semicolon)) {
            // Bodyless declaration: defined elsewhere, linked in
            // externally.
            if (!fn.is_extern_c && !fn.is_module_extern) {
                const Token& tok = peek();
                throw ParseError(tok.line, tok.column,
                                  "a function declaration without a body is only supported for "
                                  "'extern \"C\"' (ch02 §2.1) or a bare 'extern' declaration (ch11 "
                                  "§11.6); every other function must have a definition");
            }
            return fn;
        }

        if (fn.has_varargs) {
            const Token& tok = peek();
            throw ParseError(tok.line, tok.column,
                              "variadic parameters ('...') are only supported for a bodyless "
                              "'extern \"C\"' declaration, not a definition (ch02 §2.1)");
        }
        fn.body = parse_block();
        return fn;
    }

    StmtPtr parse_block() {
        SourceLocation loc = current_loc();
        expect(TokenKind::LBrace, "'{'");
        auto block = std::make_unique<Stmt>();
        block->kind = StmtKind::Block;
        block->loc = loc;
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
            block->statements.push_back(parse_statement());
        }
        expect(TokenKind::RBrace, "'}'");
        return block;
    }

    StmtPtr parse_statement() {
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
            ParsedAttributes attrs = parse_attribute_specifier_seq();
            StmtPtr stmt = parse_statement();
            if (attrs.has("unsafe") && stmt->kind == StmtKind::Block) stmt->is_unsafe = true;
            return stmt;
        }
        if (check(TokenKind::LBrace)) return parse_block();
        if (looks_like_type_start()) return parse_var_decl();
        if (check(TokenKind::KwReturn)) return parse_return();
        if (check(TokenKind::KwIf)) return parse_if();
        if (check(TokenKind::KwWhile)) return parse_while();
        return parse_expr_stmt();
    }

    StmtPtr parse_var_decl() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::VarDecl;
        stmt->loc = loc;
        if (match(TokenKind::KwAuto)) {
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
            // check_moves/codegen unresolved.
            stmt->type = Type{.kind = TypeKind::Named, .name = "auto"};
            stmt->var_name = std::string(expect(TokenKind::Identifier, "variable name").text);
            const Token& tok = peek();
            if (!match(TokenKind::Assign)) {
                throw ParseError(tok.line, tok.column,
                                  "'auto' requires an initializer ('auto name = expr;') -- there is no other "
                                  "way to know what concrete type to infer");
            }
            stmt->init = parse_expr();
            expect(TokenKind::Semicolon, "';'");
            return stmt;
        }
        Type base = parse_type(/*allow_rvalue_ref=*/false, &stmt->is_const);
        if (starts_function_pointer_declarator()) {
            stmt->type = parse_function_pointer_declarator(std::move(base), stmt->var_name);
        } else {
            stmt->var_name = std::string(expect(TokenKind::Identifier, "variable name").text);
            stmt->type = parse_array_suffix(base);
        }
        if (match(TokenKind::Assign)) {
            stmt->init = parse_expr();
        } else if (match(TokenKind::LParen)) {
            // `ClassName name(args);` (ch04 §4.2): direct-initialization
            // via an explicit constructor call -- the concrete way a
            // `class`-typed local is constructed in this version (there
            // is no `=`-initializer form for a class type yet, only this
            // or a bare, zero-initialized declaration calling no
            // constructor at all, e.g. `ClassName name;`). Movecheck/
            // codegen resolve the callee by recomputing `ClassName_new`
            // from `stmt->type`, not from anything recorded here.
            stmt->has_ctor_args = true;
            if (!check(TokenKind::RParen)) {
                do {
                    stmt->ctor_args.push_back(parse_expr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");
        }
        // A `const`-qualified local (Stmt::is_const, set above by
        // parse_type via its out_bare_const out-parameter) must be
        // initialized right here -- there is no other opportunity to
        // ever give it a value, unlike an ordinary mutable local, which
        // may be declared bare and assigned later. Matches real C++'s
        // own "default initialization of const variable" rejection.
        if (stmt->is_const && !stmt->init && !stmt->has_ctor_args) {
            throw ParseError(loc.line, loc.column,
                              "a 'const' variable must be initialized ('const " + stmt->type.name + " " +
                                  stmt->var_name + " = ...;') -- it can never be given a value afterward");
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_return() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwReturn, "'return'");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::Return;
        stmt->loc = loc;
        if (!check(TokenKind::Semicolon)) {
            stmt->expr = parse_expr();
        }
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    StmtPtr parse_if() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwIf, "'if'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::If;
        stmt->loc = loc;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        if (match(TokenKind::KwElse)) {
            stmt->else_branch = parse_statement();
        }
        return stmt;
    }

    StmtPtr parse_while() {
        SourceLocation loc = current_loc();
        expect(TokenKind::KwWhile, "'while'");
        expect(TokenKind::LParen, "'('");
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::While;
        stmt->loc = loc;
        stmt->condition = parse_expr();
        expect(TokenKind::RParen, "')'");
        stmt->then_branch = parse_statement();
        return stmt;
    }

    StmtPtr parse_expr_stmt() {
        SourceLocation loc = current_loc();
        auto stmt = std::make_unique<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->loc = loc;
        stmt->expr = parse_expr();
        expect(TokenKind::Semicolon, "';'");
        return stmt;
    }

    // Precedence climbing, lowest to highest:
    // assignment -> logic_or -> logic_and -> equality -> relational
    // -> additive -> multiplicative -> unary -> primary

    ExprPtr parse_expr() { return parse_assignment(); }

    ExprPtr parse_assignment() {
        ExprPtr lhs = parse_logic_or();
        if (match(TokenKind::Assign)) {
            ExprPtr rhs = parse_assignment();
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Binary;
            node->binary_op = BinaryOp::Assign;
            node->loc = lhs->loc;
            node->lhs = std::move(lhs);
            node->rhs = std::move(rhs);
            return node;
        }
        return lhs;
    }

    ExprPtr parse_logic_or() {
        ExprPtr lhs = parse_logic_and();
        while (check(TokenKind::PipePipe)) {
            advance();
            lhs = make_binary(BinaryOp::Or, std::move(lhs), parse_logic_and());
        }
        return lhs;
    }

    ExprPtr parse_logic_and() {
        ExprPtr lhs = parse_equality();
        while (check(TokenKind::AmpAmp)) {
            advance();
            lhs = make_binary(BinaryOp::And, std::move(lhs), parse_equality());
        }
        return lhs;
    }

    ExprPtr parse_equality() {
        ExprPtr lhs = parse_relational();
        for (;;) {
            if (match(TokenKind::EqualEqual)) {
                lhs = make_binary(BinaryOp::Eq, std::move(lhs), parse_relational());
            } else if (match(TokenKind::NotEqual)) {
                lhs = make_binary(BinaryOp::Ne, std::move(lhs), parse_relational());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_relational() {
        ExprPtr lhs = parse_additive();
        for (;;) {
            if (match(TokenKind::Less)) {
                lhs = make_binary(BinaryOp::Lt, std::move(lhs), parse_additive());
            } else if (match(TokenKind::Greater)) {
                lhs = make_binary(BinaryOp::Gt, std::move(lhs), parse_additive());
            } else if (match(TokenKind::LessEqual)) {
                lhs = make_binary(BinaryOp::Le, std::move(lhs), parse_additive());
            } else if (match(TokenKind::GreaterEqual)) {
                lhs = make_binary(BinaryOp::Ge, std::move(lhs), parse_additive());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_additive() {
        ExprPtr lhs = parse_multiplicative();
        for (;;) {
            if (match(TokenKind::Plus)) {
                lhs = make_binary(BinaryOp::Add, std::move(lhs), parse_multiplicative());
            } else if (match(TokenKind::Minus)) {
                lhs = make_binary(BinaryOp::Sub, std::move(lhs), parse_multiplicative());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_multiplicative() {
        ExprPtr lhs = parse_unary();
        for (;;) {
            if (match(TokenKind::Star)) {
                lhs = make_binary(BinaryOp::Mul, std::move(lhs), parse_unary());
            } else if (match(TokenKind::Slash)) {
                lhs = make_binary(BinaryOp::Div, std::move(lhs), parse_unary());
            } else {
                break;
            }
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        SourceLocation loc = current_loc();
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
            size_t saved_pos = pos_;
            bool parsed_as_cast = false;
            std::unique_ptr<Expr> node;
            try {
                advance(); // '('
                Type target_type = parse_type();
                if (check(TokenKind::RParen)) {
                    advance(); // ')'
                    node = std::make_unique<Expr>();
                    node->kind = ExprKind::Cast;
                    node->loc = loc;
                    node->type = std::move(target_type);
                    node->lhs = parse_unary();
                    parsed_as_cast = true;
                }
            } catch (const ParseError&) {
                // Not a type after all (e.g. `(x + y)` where `x` happens
                // to look like a type-start token but isn't followed by
                // a well-formed type) -- fall through to backtracking
                // below, same as the "no ')' immediately after" case.
            }
            if (parsed_as_cast) return node;
            pos_ = saved_pos;
        }
        if (match(TokenKind::Minus)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Neg;
            node->lhs = parse_unary();
            return node;
        }
        if (match(TokenKind::Bang)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Unary;
            node->loc = loc;
            node->unary_op = UnaryOp::Not;
            node->lhs = parse_unary();
            return node;
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
            node->lhs = parse_unary();
            return node;
        }
        if (match(TokenKind::KwDelete)) {
            if (match(TokenKind::LBracket)) {
                expect(TokenKind::RBracket, "']' after 'delete['");
                throw ParseError(loc.line, loc.column,
                                  "'delete[]' is not supported in this version yet; only scalar/object 'delete "
                                  "expr' is implemented");
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Delete;
            node->loc = loc;
            node->lhs = parse_unary();
            return node;
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
            node->lhs = parse_unary();
            return node;
        }
        return parse_postfix(parse_primary());
    }

    // Applies trailing `.name` (Member, or a method call -- ch05 §5.9 --
    // if `(` follows), `->name` (same, off a dereference -- sugar for
    // `(*p).name`, same as real C++ -- unless `p` is literally `this`,
    // see below), and `[index]` (Subscript) operators, e.g. `p.x`,
    // `arr[i]`, `p.inner.x`, `arr[i].x`, `p->x`, `obj.method(args)`.
    ExprPtr parse_postfix(ExprPtr expr) {
        for (;;) {
            if (match(TokenKind::Dot)) {
                std::string name = std::string(expect(TokenKind::Identifier, "field or method name").text);
                expr = parse_member_or_method_call(std::move(expr), name);
            } else if (match(TokenKind::Arrow)) {
                std::string name = std::string(expect(TokenKind::Identifier, "field or method name").text);
                // `this->x` (ch05 §5.9): `this` is represented as an
                // ordinary Reference-typed pseudo-parameter (see parser's
                // make_this_param), which already auto-dereferences on
                // every use (codegen_lvalue's Identifier case) exactly
                // like `a.x` already does for any other reference-typed
                // local `a` -- so unlike `p->x` for a real pointer/
                // unique_ptr `p` below, there is no separate pointee to
                // Deref through first.
                if (expr->kind == ExprKind::Identifier && expr->name == "this") {
                    expr = parse_member_or_method_call(std::move(expr), name);
                    continue;
                }
                auto deref = std::make_unique<Expr>();
                deref->kind = ExprKind::Unary;
                deref->unary_op = UnaryOp::Deref;
                deref->loc = expr->loc;
                deref->lhs = std::move(expr);
                expr = parse_member_or_method_call(std::move(deref), name);
            } else if (match(TokenKind::LBracket)) {
                ExprPtr index = parse_expr();
                expect(TokenKind::RBracket, "']'");
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
            } else if (check(TokenKind::LParen)) {
                advance(); // '('
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->loc = expr->loc;
                node->name = expr->kind == ExprKind::Lambda ? "call" : "";
                node->lhs = std::move(expr);
                if (!check(TokenKind::RParen)) {
                    do {
                        node->args.push_back(parse_expr());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "')'");
                expr = std::move(node);
            } else {
                break;
            }
        }
        return expr;
    }

    // Shared by parse_postfix's `.name`/(this-adjusted) `->name` cases:
    // `name(args)` is a method call (ch05 §5.9) -- `base` (the receiver)
    // is stored in the resulting Call's `lhs` (nullptr for an ordinary
    // free-function call, see ast.cppm's Expr), resolved to a concrete
    // synthesized function symbol only once `base`'s static type is known
    // (movecheck/codegen, not the parser). Otherwise it's a plain field
    // access, unchanged from before method calls existed.
    ExprPtr parse_member_or_method_call(ExprPtr base, const std::string& name) {
        SourceLocation loc = base->loc;
        if (match(TokenKind::LParen)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Call;
            node->loc = loc;
            node->name = name;
            node->lhs = std::move(base);
            if (!check(TokenKind::RParen)) {
                do {
                    node->args.push_back(parse_expr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')'");
            return node;
        }
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Member;
        node->loc = loc;
        node->name = name;
        node->lhs = std::move(base);
        return node;
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
    ExprPtr parse_lambda_expression() {
        SourceLocation loc = current_loc();
        expect(TokenKind::LBracket, "'['");

        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Lambda;
        node->loc = loc;

        bool first = true;
        if (!check(TokenKind::RBracket)) {
            do {
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
                    continue;
                }
                if (first && check(TokenKind::Amp) &&
                    (peek_at(1).kind == TokenKind::Comma || peek_at(1).kind == TokenKind::RBracket)) {
                    advance();
                    node->lambda_blanket_mode = LambdaCaptureMode::ByReference;
                    first = false;
                    continue;
                }
                first = false;

                if (match(TokenKind::Star)) {
                    expect(TokenKind::KwThis, "'this' (after '*' in a capture)");
                    LambdaCapture capture;
                    capture.name = "this";
                    capture.by_reference = false;
                    node->lambda_captures.push_back(std::move(capture));
                    continue;
                }
                LambdaCapture capture;
                if (match(TokenKind::KwThis)) {
                    // `[this]` -- a reference to the enclosing method's
                    // own receiver (this is scpp's *only* `this`
                    // representation, ch05 §5.9 -- there is no separate
                    // raw-pointer form to distinguish from).
                    capture.name = "this";
                    capture.by_reference = true;
                    node->lambda_captures.push_back(std::move(capture));
                    continue;
                }
                capture.by_reference = match(TokenKind::Amp);
                capture.name = std::string(expect(TokenKind::Identifier, "captured variable name").text);
                if (match(TokenKind::Assign)) {
                    // Init-capture: `[name = expr]`/`[&name = expr]` --
                    // `expr` is evaluated in the *enclosing* scope; how
                    // a move-only type crosses into a closure (ch05
                    // §5.12), e.g. `[p = std::move(p)]`.
                    capture.init = parse_expr();
                }
                node->lambda_captures.push_back(std::move(capture));
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RBracket, "']'");

        node->lambda_params = parse_param_list();
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
                throw ParseError(current_loc().line, current_loc().column,
                                  "a generic (concept-constrained) parameter is not supported on a lambda "
                                  "parameter list in this version (ch05 §5.11 -- only a free function may be "
                                  "generic); a bare 'auto' parameter is fine");
            }
        }

        node->lambda_is_mutable = match(TokenKind::KwMutable);

        if (match(TokenKind::Arrow)) {
            node->type = parse_type();
            node->has_lambda_explicit_return_type = true;
        }

        node->lambda_body = parse_block();
        return node;
    }

    ExprPtr parse_primary() {
        const Token& tok = peek();
        SourceLocation loc{tok.line, tok.column};

        if (check(TokenKind::LBracket)) {
            return parse_lambda_expression();
        }

        if (check_std_qualified("move")) {
            consume_std_qualified();
            expect(TokenKind::LParen, "'('");
            ExprPtr inner = parse_expr();
            expect(TokenKind::RParen, "')'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Move;
            node->loc = loc;
            node->lhs = std::move(inner);
            return node;
        }

        if (check_scpp_qualified("is_thread_movable") || check_scpp_qualified("is_thread_shareable")) {
            bool movable = check_scpp_qualified("is_thread_movable");
            advance(); // scpp
            expect(TokenKind::ColonColon, "'::'");
            advance(); // is_thread_*
            expect(TokenKind::LParen, "'('");
            Type queried = parse_type();
            expect(TokenKind::RParen, "')'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::TypeTrait;
            node->loc = loc;
            node->name = movable ? "is_thread_movable" : "is_thread_shareable";
            node->type = std::move(queried);
            return node;
        }

        if (match(TokenKind::KwNew)) {
            Type element_type = parse_type();
            if (match(TokenKind::LBracket)) {
                expect(TokenKind::RBracket, "']' after 'new T['");
                throw ParseError(loc.line, loc.column,
                                  "'new T[n]' is not supported in this version yet; only scalar/object 'new T' "
                                  "and 'new T(args...)' are implemented");
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::New;
            node->loc = loc;
            node->type = std::move(element_type);
            if (match(TokenKind::LParen)) {
                node->has_paren_init = true;
                if (!check(TokenKind::RParen)) {
                    do {
                        node->args.push_back(parse_expr());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "')'");
            }
            return node;
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
            Type target_type = parse_type();
            expect(TokenKind::Greater, "'>'");
            expect(TokenKind::LParen, "'('");
            ExprPtr operand = parse_expr();
            expect(TokenKind::RParen, "')'");
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Cast;
            node->loc = loc;
            node->type = std::move(target_type);
            node->lhs = std::move(operand);
            return node;
        }

        if (match(TokenKind::IntegerLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::IntegerLiteral;
            node->loc = loc;
            node->int_value = std::stoll(std::string(tok.text));
            return node;
        }
        if (match(TokenKind::FloatLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::FloatLiteral;
            node->loc = loc;
            node->float_value = std::stod(std::string(tok.text));
            return node;
        }
        if (match(TokenKind::CharLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::CharLiteral;
            node->loc = loc;
            node->int_value = decode_char_literal(tok);
            return node;
        }
        if (match(TokenKind::StringLiteral)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::StringLiteral;
            node->loc = loc;
            node->name = decode_string_literal(tok);
            return node;
        }
        if (match(TokenKind::KwThis)) {
            // ch05 §5.9: `this` is a keyword (not an ordinary identifier
            // -- so a user can never accidentally shadow it with a
            // same-named parameter/local), but behaves exactly like an
            // Identifier expression bound to the name "this" everywhere
            // downstream: it resolves through the exact same
            // `body.local_types`/`locals_` lookup as any other reference-
            // typed local, since parse_class_def's make_this_param
            // already registered it as an ordinary params[0] named
            // "this".
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            node->name = "this";
            return node;
        }
        if (match(TokenKind::KwTrue)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = true;
            return node;
        }
        if (match(TokenKind::KwFalse)) {
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::BoolLiteral;
            node->loc = loc;
            node->bool_value = false;
            return node;
        }
        if (check(TokenKind::Identifier)) {
            // ch11: may be a plain name or a namespace-qualified one
            // (`std::string`, `a::b::foo`) -- parse_qualified_name
            // consumes the whole `::`-joined chain (a lone identifier,
            // the overwhelmingly common case, is just a chain of length
            // one).
            std::string name = parse_qualified_name();
            // ch05 §5.11: `name<Args>(...)` -- an explicit-template-
            // argument call to a known full-header-form generic
            // function (e.g. `make<Circle>()`, `get<2>(t)`) --
            // recognized structurally only when `name` is already a
            // declared generic-function-template name (mirrors how a
            // generic *type* instantiation, `Name<Arg>`, is
            // disambiguated in parse_unqualified_type), avoiding any
            // ambiguity with an ordinary `a < b` comparison for every
            // other identifier.
            auto generic_fn_it = generic_function_template_params_.find(name);
            std::vector<ExplicitTemplateArg> explicit_template_args;
            if (generic_fn_it != generic_function_template_params_.end() && check(TokenKind::Less)) {
                advance(); // '<'
                const std::vector<GenericTypeParam>& fn_template_params = generic_fn_it->second;
                size_t arg_index = 0;
                if (!check(TokenKind::Greater)) {
                    do {
                        ExplicitTemplateArg arg;
                        bool is_non_type =
                            arg_index < fn_template_params.size() && fn_template_params[arg_index].is_non_type;
                        if (is_non_type) {
                            arg.is_type = false;
                            arg.value = std::shared_ptr<Expr>(parse_additive().release());
                        } else {
                            arg.is_type = true;
                            arg.type = parse_type();
                        }
                        explicit_template_args.push_back(std::move(arg));
                        arg_index++;
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::Greater, "'>'");
            }
            if (match(TokenKind::LParen)) {
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Call;
                node->loc = loc;
                node->name = name;
                node->explicit_template_args = std::move(explicit_template_args);
                if (!check(TokenKind::RParen)) {
                    do {
                        node->args.push_back(parse_expr());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "')'");
                return node;
            }
            auto node = std::make_unique<Expr>();
            node->kind = ExprKind::Identifier;
            node->loc = loc;
            node->name = name;
            return node;
        }
        if (match(TokenKind::LParen)) {
            size_t saved_pos = pos_;
            if (match(TokenKind::Ellipsis)) {
                std::optional<BinaryOp> op = parse_fold_operator();
                if (!op.has_value()) {
                    const Token& bad = peek();
                    throw ParseError(bad.line, bad.column,
                                      "expected a fold operator after '...' (ch05 §5.11)");
                }
                ExprPtr pack = parse_unary();
                expect(TokenKind::RParen, "')'");
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Fold;
                node->loc = loc;
                node->binary_op = *op;
                node->fold_ellipsis_on_left = true;
                node->lhs = std::move(pack);
                return node;
            }
            pos_ = saved_pos;
            ExprPtr first = parse_unary();
            if (std::optional<BinaryOp> op = parse_fold_operator(); op.has_value() && check(TokenKind::Ellipsis)) {
                advance(); // '...'
                auto node = std::make_unique<Expr>();
                node->kind = ExprKind::Fold;
                node->loc = loc;
                node->binary_op = *op;
                node->lhs = std::move(first);
                if (std::optional<BinaryOp> trailing = parse_fold_operator()) {
                    if (*trailing != *op) {
                        const Token& bad = peek();
                        throw ParseError(bad.line, bad.column,
                                          "a binary fold expression must use the same operator on both sides of "
                                          "'...' (ch05 §5.11)");
                    }
                    node->rhs = parse_unary();
                }
                expect(TokenKind::RParen, "')'");
                return node;
            }
            pos_ = saved_pos;
            ExprPtr inner = parse_expr();
            expect(TokenKind::RParen, "')'");
            return inner;
        }

        throw ParseError(tok.line, tok.column, "expected an expression but found '" +
                                                    std::string(tok.text) + "'");
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

    std::optional<BinaryOp> parse_fold_operator() {
        if (match(TokenKind::Plus)) return BinaryOp::Add;
        if (match(TokenKind::Minus)) return BinaryOp::Sub;
        if (match(TokenKind::Star)) return BinaryOp::Mul;
        if (match(TokenKind::Slash)) return BinaryOp::Div;
        if (match(TokenKind::EqualEqual)) return BinaryOp::Eq;
        if (match(TokenKind::NotEqual)) return BinaryOp::Ne;
        if (match(TokenKind::Less)) return BinaryOp::Lt;
        if (match(TokenKind::Greater)) return BinaryOp::Gt;
        if (match(TokenKind::LessEqual)) return BinaryOp::Le;
        if (match(TokenKind::GreaterEqual)) return BinaryOp::Ge;
        if (match(TokenKind::AmpAmp)) return BinaryOp::And;
        if (match(TokenKind::PipePipe)) return BinaryOp::Or;
        return std::nullopt;
    }
};

Program parse(std::vector<Token> tokens, const ModuleResolver& resolver = {},
              const PartitionResolver& partition_resolver = {}) {
    Parser parser(std::move(tokens), resolver, partition_resolver);
    return parser.parse_program();
}

Program parse(std::string_view source, const ModuleResolver& resolver = {},
              const PartitionResolver& partition_resolver = {}) {
    return parse(tokenize(source), resolver, partition_resolver);
}

} // namespace scpp
