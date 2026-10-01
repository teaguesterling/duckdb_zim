#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
// ConstantExpression is not reachable through duckdb.hpp; the path is identical
// on the v1.5 and v2.0 lines, so a direct include needs no probe of its own.
#include "duckdb/parser/expression/constant_expression.hpp"
#include <type_traits>
#include <utility>

// Compatibility shims for building against BOTH the pinned stable DuckDB
// (v1.5.x, what this extension ships against) and DuckDB main (the v2.0 line,
// what community-extensions' `test_against_latest` builds against).
//
// FEATURE DETECTION, NOT VERSION NUMBERS. A version macro says when a thing
// changed; a probe says whether it changed here. The probe keeps working when a
// change is backported, reverted, or lands on a different branch than expected.
// (Idiom borrowed from duckdb_webbed's duckdb_compat.hpp, which the rest of the
// duck_block ecosystem already uses.)

// duckdb::Identifier replaced std::string as the name type in table-function and
// COPY bind signatures. Identifier compares case-insensitively, and construction
// from a RUNTIME string is explicit by design -- promoting a string to an
// identifier is meant to be a deliberate act at the call site -- so a boundary
// helper is needed rather than an implicit conversion.
#if __has_include("duckdb/common/identifier.hpp")
#define DUCKDB_HAS_IDENTIFIER 1
#include "duckdb/common/identifier.hpp"
#endif

// v2.0 split the per-vector accessor classes out of
// duckdb/common/types/vector.hpp into one header each under
// duckdb/common/vector/, and duckdb.hpp no longer pulls them in transitively.
// This header names FlatVector at namespace scope, so include it explicitly
// rather than relying on a transitive include that a given TU may not have.
// Presents otherwise as "'FlatVector' has not been declared", which reads like
// a missing symbol rather than a moved header.
#if __has_include("duckdb/common/vector/flat_vector.hpp")
#include "duckdb/common/vector/flat_vector.hpp"
#endif
#if __has_include("duckdb/common/vector/list_vector.hpp")
#include "duckdb/common/vector/list_vector.hpp"
#endif
#if __has_include("duckdb/common/vector/struct_vector.hpp")
#include "duckdb/common/vector/struct_vector.hpp"
#endif

namespace duckdb {

// --- bind-signature name type -------------------------------------------------
// Used wherever a bind callback receives or fills a vector of column names.
//
// DERIVED FROM DUCKDB'S OWN CONTAINER, NOT FROM A HEADER PROBE. The obvious
// probe -- #ifdef DUCKDB_HAS_IDENTIFIER -- is a TIME BOMB, because the presence
// of identifier.hpp and the type used in bind signatures are two different
// facts that have already come apart upstream:
//
//   v1.5-variegata @ b155d6f63c (our pin)  no identifier.hpp   bind: vector<string>
//   v1.5-variegata @ branch tip            HAS identifier.hpp  bind: vector<string>
//   main (v2.0)                            HAS identifier.hpp  bind: vector<Identifier>
//
// identifier.hpp was BACKPORTED to the stable branch without changing
// table_function_bind_t. So on the next submodule bump a header probe flips
// CompatName to Identifier on a DuckDB that still wants strings, and every bind
// signature in the extension stops compiling at once.
//
// Read the name type off table_function_bind_t ITSELF -- its fourth parameter is
// the vector being ported. Tighter than deriving from
// TableFunctionBindInput::input_table_names, which is a sibling that happens to
// move in step; here there is no "happens to" left, because the typedef named is
// the one that changed. Every bind signature then follows automatically.
template <class T>
struct CompatBindNamesOf;
template <class R, class A, class B, class C, class D>
struct CompatBindNamesOf<R (*)(A, B, C, D)> {
	using type = typename std::remove_reference<D>::type::value_type;
};
using CompatName = CompatBindNamesOf<table_function_bind_t>::type;

inline string CompatNameStr(const string &name) {
	return name;
}
#ifdef DUCKDB_HAS_IDENTIFIER
// Only declares the Identifier overload; it does NOT decide CompatName. Both
// overloads coexist happily when CompatName is still string.
inline string CompatNameStr(const Identifier &id) {
	return id.GetIdentifierName();
}
#endif

inline CompatName CompatMakeName(string name) {
	return CompatName(std::move(name));
}

// Ties the derived type to its overload set. Deriving CompatName fixes the type
// but leaves a second failure mode open: CompatName could resolve to Identifier
// on a DuckDB whose identifier.hpp this header did not find, so the Identifier
// overload above was never declared -- and then CompatNameStr either fails to
// match or silently picks a worse conversion. Assert the coupling instead of
// assuming it.
static_assert(std::is_same<decltype(CompatNameStr(std::declval<const CompatName &>())), string>::value,
              "CompatNameStr must accept the derived CompatName on every DuckDB line");

// --- LogicalType alias ---------------------------------------------------------
// v1.5: void SetAlias(string)      -- mutates in place
// v2.0: LogicalType WithAlias(string) const -- returns a copy, never mutating a
//       type whose type-info is shared.
//
// Detected by PROBING for the member rather than by the Identifier macro above,
// because these are two independent changes and tying one to the other would
// silently pick the wrong branch if they ever land in different releases.
// The member probe itself (the decltype(void(expr)) partial specialisation) is
// valid C++11; only the dispatch below needs care -- see the note on it.
template <class T, class = void>
struct CompatHasWithAlias : std::false_type {};
template <class T>
struct CompatHasWithAlias<T, decltype(void(std::declval<const T &>().WithAlias(string())))> : std::true_type {};

// Dispatched on a tag rather than with `if constexpr`, so the header also
// compiles at C++11. Several extensions in this ecosystem build their TUs at
// C++11 deliberately (forcing C++17 on the extension but not on libduckdb makes
// static-const members in duckdb's headers acquire implicit inline linkage in
// one and not the other, which produces multiple-definition link errors), and
// `if constexpr` is C++17-only. Tag dispatch has the same property that matters
// here: only the selected overload is instantiated, so the branch referring to
// the absent member is never compiled.
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::true_type) {
	return type.WithAlias(std::move(alias));
}
template <class TYPE>
inline LogicalType CompatWithAliasImpl(TYPE type, string alias, std::false_type) {
	type.SetAlias(std::move(alias));
	return type;
}
// The ENTRY POINT is deliberately NOT a template. A `template <class TYPE =
// LogicalType>` form looks equivalent but is not: the default template argument
// is inert because deduction wins, so the very common call
//
//     CompatWithAlias(LogicalType::VARCHAR, "md")
//
// deduces TYPE = LogicalTypeId -- `LogicalType::VARCHAR` is a static constexpr
// LogicalTypeId (types.hpp), not a LogicalType -- and then hard-errors inside
// the shim with "request for member 'SetAlias' in 'type', which is of non-class
// type 'duckdb::LogicalTypeId'". A concrete parameter restores the implicit
// LogicalTypeId -> LogicalType conversion at the call site. Only the Impl
// overloads stay templated, which is all the tag dispatch needs.
inline LogicalType CompatWithAlias(LogicalType type, string alias) {
	return CompatWithAliasImpl(std::move(type), std::move(alias), CompatHasWithAlias<LogicalType>());
}

// --- Vector::ToUnifiedFormat ---------------------------------------------------
// v1.5: ToUnifiedFormat(count, data)  -- the only overload
// v2.0: ToUnifiedFormat(data)         -- plus the count form kept as [[deprecated]]
//
// PROBE FOR THE COUNT-FREE OVERLOAD, not the count-taking one. v2.0 did not
// remove the count form, it deprecated it, so a probe for the count form is
// true on BOTH versions and the shim would always take the deprecated path --
// silently never calling the new API it exists to reach. The count-free form is
// the one that exists only on v2.0, so it is the one that discriminates.
template <class T, class = void>
struct CompatToUnifiedWithoutCount : std::false_type {};
template <class T>
struct CompatToUnifiedWithoutCount<T, decltype(void(std::declval<T &>().ToUnifiedFormat(
                                          std::declval<UnifiedVectorFormat &>())))> : std::true_type {};

template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t, UnifiedVectorFormat &data, std::true_type) {
	vec.ToUnifiedFormat(data);
}
template <class VEC>
inline void CompatToUnifiedFormatImpl(VEC &vec, idx_t count, UnifiedVectorFormat &data, std::false_type) {
	vec.ToUnifiedFormat(count, data);
}
template <class VEC = Vector>
inline void CompatToUnifiedFormat(VEC &vec, idx_t count, UnifiedVectorFormat &data) {
	CompatToUnifiedFormatImpl(vec, count, data, CompatToUnifiedWithoutCount<VEC>());
}

// --- FlatVector mutable data ---------------------------------------------------
// v1.5: FlatVector::GetData<T>(vec)         returns T*
// v2.0: FlatVector::GetData<T>(vec)         returns const T*
//       FlatVector::GetDataMutable<T>(vec)  returns T*
// Writing through the v2.0 GetData is a compile error, which is the point of the
// split -- so the WRITE path must ask for mutability explicitly.
template <class T, class = void>
struct CompatHasFlatGetDataMutable : std::false_type {};
template <class T>
struct CompatHasFlatGetDataMutable<T, decltype(void(T::template GetDataMutable<bool>(std::declval<Vector &>())))>
    : std::true_type {};

template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::true_type) {
	return FV::template GetDataMutable<VALUE>(vec);
}
template <class VALUE, class FV>
inline VALUE *CompatFlatDataMutableImpl(Vector &vec, std::false_type) {
	return FV::template GetData<VALUE>(vec);
}
template <class VALUE, class FV = FlatVector>
inline VALUE *CompatFlatDataMutable(Vector &vec) {
	return CompatFlatDataMutableImpl<VALUE, FV>(vec, CompatHasFlatGetDataMutable<FV>());
}

// --- FlatVector validity mask -----------------------------------------------
// v1.5: Validity(Vector &)              returns ValidityMask &
// v2.0: Validity(const Vector &)        returns const ValidityMask &
//       ValidityMutable(Vector &)       returns ValidityMask &
//
// Same copy-on-write split as GetData/GetDataMutable: ValidityMutable goes
// through BufferMutable() and un-shares, Validity goes through Buffer() and does
// not. Probed separately from the GetDataMutable change because they are two
// independent upstream changes.
//
// Worse to diagnose than the GetData case: `auto &m = FlatVector::Validity(v)`
// still COMPILES on v2.0, silently deducing a const reference. The error appears
// later, at the mutation, as "passing 'const duckdb::ValidityMask' as 'this'
// argument discards qualifiers" -- naming neither Validity nor FlatVector. Grep
// for the MUTATION (SetInvalid/SetValid/SetAllInvalid/SetAllValid) and walk back
// to where the reference was bound.
template <class T, class = void>
struct CompatHasFlatValidityMutable : std::false_type {};
template <class T>
struct CompatHasFlatValidityMutable<T, decltype(void(T::ValidityMutable(std::declval<Vector &>())))> : std::true_type {
};

template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::true_type) {
	return FV::ValidityMutable(vec);
}
template <class FV>
inline ValidityMask &CompatFlatValidityMutableImpl(Vector &vec, std::false_type) {
	return FV::Validity(vec);
}
template <class FV = FlatVector>
inline ValidityMask &CompatFlatValidityMutable(Vector &vec) {
	return CompatFlatValidityMutableImpl<FV>(vec, CompatHasFlatValidityMutable<FV>());
}

// --- finishing a chunk: SetCardinality vs SetChildCardinality -------------------
// v1.5: SetCardinality(n)        sets the chunk's count. Vectors have no own size.
// v2.0: SetCardinality(n)        [[deprecated]], sets ONLY the chunk's count
//       SetChildCardinality(n)   also sizes the CHILD VECTORS
//
// v2.0 gave every Vector its own size, and that size is what several operators now
// iterate. `Vector::SetValue(i, v)` writes AT AN INDEX and never moves it, so a
// table function that fills a chunk with SetValue -- the idiomatic shape, and the
// only shape this extension uses -- leaves every child vector at size 0 after
// DataChunk::Reset(). SetChildCardinality is the only call that sizes them.
//
// THE FAILURE IS SILENT AND VERY MISLEADING. Nothing crashes and most things stay
// correct, because the printer, scalar functions and aggregates all work off the
// CHUNK's count. But `IS NULL` / `IS NOT NULL` go through IsNullLoop
// (null_operations.cpp), which iterates `input.size()` -- the VECTOR's size. At
// zero it writes no results at all and the caller reads the untouched result
// buffer, so every row reports false. The observable symptom is a column that
// PRINTS as NULL while `x IS NULL` returns false for the very same row, in the
// very same result set: read_zim.test:49 and zim_include_content.test:21 are
// exactly this. Reading the value is fine; asking whether it is null is not.
//
// Probed on SetChildCardinality, which exists only on v2.0. Note the v1.5 branch
// must not be spelled `SetChildCardinality` anywhere the compiler can see it on
// v1.5, hence the tag dispatch rather than an `if constexpr`.
//
// Only correct because this codebase never uses Vector::Append/AppendValue, which
// DO advance the vector size as they go; for an appending caller
// SetChildCardinality(N) would be a no-op at best and a logical shrink at worst.
// The audit is `grep -rn '\.Append(\|AppendValue' src/`, and it is empty here.
template <class T, class = void>
struct CompatHasSetChildCardinality : std::false_type {};
template <class T>
struct CompatHasSetChildCardinality<T, decltype(void(std::declval<T &>().SetChildCardinality(idx_t(0))))>
    : std::true_type {};

template <class CHUNK>
inline void CompatSetCardinalityImpl(CHUNK &chunk, idx_t count, std::true_type) {
	chunk.SetChildCardinality(count);
}
template <class CHUNK>
inline void CompatSetCardinalityImpl(CHUNK &chunk, idx_t count, std::false_type) {
	chunk.SetCardinality(count);
}
template <class CHUNK = DataChunk>
inline void CompatSetCardinality(CHUNK &chunk, idx_t count) {
	CompatSetCardinalityImpl(chunk, count, CompatHasSetChildCardinality<CHUNK>());
}

// ConstantExpression(const Value &) is DELETED on v2.0, which names its own
// replacement in the deletion:
//
//     DUCKDB_API explicit ConstantExpression(Literal literal);
//     //! Values are not literals - use ConstantExpression::FromValue
//     explicit ConstantExpression(const Value &value) = delete;
//     ...
//     DUCKDB_API static unique_ptr<ParsedExpression> FromValue(const Value &);
//
// The v1.5 line has `explicit ConstantExpression(Value)` and NO FromValue, so
// neither spelling compiles on both and the replacement is not a rename: v2.0's
// FromValue returns a ParsedExpression that may be a literal, a constructor call
// for a nested value, or a cast. Our call site pushes into a
// vector<unique_ptr<ParsedExpression>>, so the richer return type slots in.
//
// PROBE THE THING THAT CHANGED, per this file's own rule: ask whether
// ConstantExpression::FromValue exists, not whether some header is present or
// some version macro is set. FromValue IS the migration DuckDB added; a probe on
// it cannot answer for a different change the way a header probe can (a header probe
// such as the __has_include(identifier.hpp) check above answers for a file's
// presence, not for whether this constructor was deleted).
template <class T, class = void>
struct CompatHasFromValue : std::false_type {};
template <class T>
struct CompatHasFromValue<T, decltype(void(T::FromValue(std::declval<const Value &>())))> : std::true_type {};

template <class CE>
inline unique_ptr<ParsedExpression> CompatConstantImpl(Value value, std::true_type) {
	return CE::FromValue(value);
}
template <class CE>
inline unique_ptr<ParsedExpression> CompatConstantImpl(Value value, std::false_type) {
	return make_uniq<CE>(std::move(value));
}

//! A parsed expression for a literal value, on either DuckDB line.
template <class CE = ConstantExpression>
inline unique_ptr<ParsedExpression> CompatConstant(Value value) {
	return CompatConstantImpl<CE>(std::move(value), CompatHasFromValue<CE>());
}

// BOTH ANSWERS ARE PINNED, not just the one our pin happens to give. A detector
// tested only against the line you build on is a detector you have half-checked:
// it would pass identically if it always returned false, which is precisely the
// answer v1.5 wants and v2.0 does not. These cost nothing at runtime and fail
// the build the day the probe stops discriminating.
namespace compat_detail {
//! Shaped like v2.0's ConstantExpression: Value constructor deleted, FromValue present.
struct HasFromValueProbe {
	explicit HasFromValueProbe(const Value &) = delete;
	static unique_ptr<ParsedExpression> FromValue(const Value &);
};
//! Shaped like v1.5's: a Value constructor and no FromValue.
struct NoFromValueProbe {
	explicit NoFromValueProbe(Value);
};
static_assert(CompatHasFromValue<HasFromValueProbe>::value,
              "CompatHasFromValue must detect FromValue where it exists (the v2.0 shape)");
static_assert(!CompatHasFromValue<NoFromValueProbe>::value,
              "CompatHasFromValue must not fire where FromValue is absent (the v1.5 shape)");
} // namespace compat_detail

// --- Table function named parameters -----------------------------------------
// v1.5: TableFunction::named_parameters, a flat map keyed by string.
// v2.0: that member is GONE. Named parameters moved onto FunctionSignature and
//       are declared as TYPED KWARGS:
//         func.GetSignature().WithTypedKwargs("options", [](TypedKwargs &k) {
//             k.Add(Identifier("include_content"), LogicalType::ANY); ... });
//       (the migration landed with d43fcb8ce1 "Add BoundTableFunction and use
//       FunctionSignature", 5eb0f76e48 "unify binding", cb4ab4a105; it reached
//       v2.0-cyanoptera around 2026-09-28)
//
// SENTINEL: duckdb/main/capi/capi_function_signature.hpp. Verified to co-vary
// with the API across the two commits this extension builds against -- ABSENT on
// v1.5.6, PRESENT on v2.0-cyanoptera. Deliberately NOT these two:
//   - duckdb/common/identifier.hpp        is present on BOTH lines (backported to
//                                         v1.5.x), so it never discriminates
//   - .../identifier_case_mode.hpp        is v2.0-only and so LOOKS valid, but it
//                                         landed BEFORE TableFunction::GetSignature;
//                                         selecting on it produced a documented
//                                         99-error mis-selection in sitting_duck
// Ported from teaguesterling/sitting_duck's named_parameter_compat.hpp (the
// reference implementation, verified green on cyanoptera 96063b9e). Names match
// the fleet convention agreed with duckdb_markdown so cross-repo review reads
// the same; the header stays local, per that repo's own derivation discipline.
#if __has_include("duckdb/main/capi/capi_function_signature.hpp")
#define DUCKDB_HAS_TYPED_KWARGS 1
#endif

//! One named parameter: the name callers write, and the type it accepts.
struct CompatNamedParamSpec {
	const char *name;
	LogicalType type;
};

//! The kwargs group v2.0 collects a table function's options under. v1.5 has no
//! grouping and ignores this.
static constexpr const char *COMPAT_KWARGS_GROUP = "options";

//! The type TableFunctionBindInput::named_parameters actually hands out.
//!
//! SPELL THIS, never named_parameter_map_t directly. v2.0 STILL DEFINES that
//! name (as identifier_map_t) while the bind input hands out
//! named_argument_map_t -- so naming it compiles on both lines and silently
//! means a DIFFERENT TYPE on v2.0. zim's three bind loops iterate with `auto`
//! and so dodge this, but any signature that receives the map must use the alias.
#ifdef DUCKDB_HAS_TYPED_KWARGS
using CompatNamedParamMap = named_argument_map_t;
#else
using CompatNamedParamMap = named_parameter_map_t;
#endif

#ifdef DUCKDB_HAS_TYPED_KWARGS

//! Declare a table function's named parameters. Call ONCE per function, then
//! CompatExtendNamedParams for any further ones: v2.0 separates creating the
//! kwargs parameter from adding options to it, and ExtendTypedKwargs THROWS if
//! the signature has no kwargs parameter yet.
inline void CompatDeclareNamedParams(TableFunction &func, const vector<CompatNamedParamSpec> &params) {
	func.GetSignature().WithTypedKwargs(Identifier(COMPAT_KWARGS_GROUP), [&params](TypedKwargs &kwargs) {
		for (const auto &param : params) {
			// Identifier(const string &) is EXPLICIT, so a runtime name must be
			// promoted deliberately rather than passed as a literal.
			kwargs.Add(Identifier(string(param.name)), param.type);
		}
	});
}

//! Add further named parameters to a function that has already declared some.
inline void CompatExtendNamedParams(TableFunction &func, const vector<CompatNamedParamSpec> &params) {
	func.GetSignature().ExtendTypedKwargs([&params](TypedKwargs &kwargs) {
		for (const auto &param : params) {
			kwargs.Add(Identifier(string(param.name)), param.type);
		}
	});
}

//! The value bound to `name`, or nullptr when the call omitted it.
inline const Value *CompatFindNamedParam(const named_argument_map_t &params, const char *name) {
	// find(), NOT count(): named_argument_map_t has no count().
	auto entry = params.find(Identifier(string(name)));
	if (entry == params.end()) {
		return nullptr;
	}
	return &entry->second;
}

#else

//! Declare a table function's named parameters (v1.5: the flat map).
inline void CompatDeclareNamedParams(TableFunction &func, const vector<CompatNamedParamSpec> &params) {
	for (const auto &param : params) {
		func.named_parameters[param.name] = param.type;
	}
}

//! On v1.5 the map does not care when a key arrives, so this is Declare. The
//! call sites keep the same shape on both lines regardless.
inline void CompatExtendNamedParams(TableFunction &func, const vector<CompatNamedParamSpec> &params) {
	CompatDeclareNamedParams(func, params);
}

//! The value bound to `name`, or nullptr when the call omitted it.
inline const Value *CompatFindNamedParam(const named_parameter_map_t &params, const char *name) {
	auto entry = params.find(name);
	if (entry == params.end()) {
		return nullptr;
	}
	return &entry->second;
}

#endif

//! The value bound to `name`, throwing when absent -- the shape map::at had, for
//! call sites that already knew the parameter was present.
template <class MAP>
inline const Value &CompatNamedParamAt(const MAP &params, const char *name) {
	auto value = CompatFindNamedParam(params, name);
	if (!value) {
		throw InvalidInputException("named parameter '%s' was not provided", name);
	}
	return *value;
}

//! Whether the call provided `name` at all.
template <class MAP>
inline bool CompatHasNamedParam(const MAP &params, const char *name) {
	return CompatFindNamedParam(params, name) != nullptr;
}

// NO `{}` CATCH-ALL BRANCH, deliberately. A third fallback that quietly declares
// nothing would compile against ANY DuckDB and register an empty parameter set:
// every named parameter silently stops binding, the docs lose their parameters,
// and nothing goes red. If the sentinel ever stops discriminating, this header
// must FAIL TO COMPILE rather than succeed at doing nothing -- which is what the
// two branches above, and only two branches, guarantee.
//
// VERIFY BY VALUE, NOT BY BINDING. "the function still binds" passes even when a
// parameter is declared with the wrong type or dropped. Pick a parameter whose
// value visibly changes output on BOTH lines -- e.g. zim_search's max_results --
// and diff the result across the two builds.

} // namespace duckdb
