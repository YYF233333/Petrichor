# A New Scripting Language for Godot: Specification

**v0.1 · 2026-10-21 · Public draft for comment** · 中文：[language-spec.zh.md](language-spec.zh.md)

This document specifies the semantics of a statically typed scripting language for Godot, intended as a replacement for GDScript. Only the specification exists for now; there is no implementation yet. The language has no final name and is called "the language" below. The Chinese and English versions are kept in sync.

## 1. Overview

### 1.1 Scope

- A new scripting language for Godot. It keeps scripting low-friction while adding strong static typing and type inference.
- It reuses Godot core's basic types and infrastructure. To the language these are external types; the engine defines how they live, how they are copied, and their ABI contracts.
- Reusing Godot's semantics does not imply source compatibility with GDScript.
- It ships as an **out-of-tree Godot module**. It starts with a typed IR interpreter; a JIT can come later. Neither a GDExtension compatibility layer nor changes to core are prerequisites. Writing scripts does not require users to install a toolchain.
- There is no hot reload and no migration of live state; stopping and re-running is the normal workflow.

### 1.2 Design principles

1. **Users shouldn't have to worry about performance, but the semantics must be spelled out.** Users write few type or memory-management annotations. In exchange, assignment, sharing, conversion, invalidation and errors must all have a precise meaning. When a type cannot be inferred, that is an error; the language never falls back to dynamic typing.
2. **Semantics first, algorithm second.** The rules specify which programs are valid and what type each thing has. They are not tied to any particular inference algorithm.
3. **What the engine can observe is semantics.** GDScript, the editor and resource files can all see a value's shape inside a Variant, how it compares for equality, and how it is saved. So the specification defines these.
4. **Strict first.** A restriction can be relaxed later without breaking anyone; adding one later breaks existing code. When in doubt, choose the stricter option.
5. **Don't optimize for hypothetical needs.** Capabilities without a clear demand are listed as known limitations or future work.

### 1.3 Conventions

- The main text is normative. These markers flag non-normative status:
  - **[Open]**: the direction is settled, but syntax or details are not.
  - **[Known limitation]**: explicitly not supported in the first version.
  - **[Later]**: planned, but not blocking the first version.
- Syntax in code examples is tentative; the actual spelling is [Open].
- "Program error" refers to the class of runtime errors defined in §10.
- All engine behavior cited here was checked against Godot master (4.8-beta, commit `469659e`). Sources are listed in Appendix A.

## 2. Type system overview

1. Strong static typing, inferred by default. If a type cannot be inferred, it is an error; there is no silent fallback to `any`.
2. `any` holds a value of any language type. Operations that require a concrete type require a type check, narrowing or conversion (§2.1).
3. Method calls and operators produce capability constraints. Traits give these constraints names so they can be reused (§4).
4. There are named enums with payloads and exhaustive pattern matching. Option and Result are built from the same mechanism (§6).
5. Conditions must be bool. There is no general truthiness.
6. Single inheritance with overriding; no multiple inheritance. An override must honor the parent method's contract and must not narrow the parameters it accepts. Engine callbacks get their signatures from the engine contract.
7. No user-defined overloading. Default parameters, generics and overriding are kept. Engine methods that have several call forms are adapted by the binding layer.
8. The surface syntax uses Python/GDScript-style indentation and colons.

### 2.1 The any type

An `any` value contains a dynamic type and a value of that type. Basic types and user-defined types, including enums and Option, can be stored in `any`. Numeric values are normalized on entry: i8…i64 and u8…u32 become i64, and f32 and f64 become f64. This preserves the numeric value, not the source width. A u64 retains its full unsigned value and dynamic type in a typed box, for all values (§12.1). An `any` value is non-null: absence is represented by `Option[any].None`, not by an empty `any`.

`any` is not a base class. Converting an Object reference to `any` does not change the Object inheritance hierarchy or its ownership and liveness rules (§5). A value stored in `any` must be checked, narrowed or converted before an operation requiring a concrete type. Assigning an `any` value to another `any` copies the contained value; it does not add another dynamic container layer.

The representation of `any` is specified in §12.1. It is compatible with the Godot Variant ABI, but its set of valid values excludes bare NIL. A language value of type `Option[T]` stored in `any` retains that type and its `None` or `Some` variant; it is distinct from absence of the enclosing `any`.

## 3. Type inference

Inference is defined by rules R1–R7. Under these rules, an implementation needs only local inference within each function, plus joint solving of return types and members across declarations (§11). It needs no let-polymorphism, no value restriction and no anonymous structural constraints.

### R1 Types are not determined by callers

The type of a declaration is determined only by the declaration itself and its body. With mutual recursion, it is determined together with the strongly connected component (SCC) it belongs to. Call sites play no part.

Rationale: if types were inferred from callers, a function's type would change every time someone adds a call. In addition, some callers cannot be seen by static analysis at all, such as `call()`, signals connected in the editor, and AnimationPlayer method tracks.

Exception: a lambda's parameter types may come from the expected type at the place where the lambda is written. This is purely local and does not violate R1.

### R2 Anything the engine can call is monomorphic; generics must be explicit

Class methods, signals and member properties are all entry points for the engine. `call()`, `has_method`, signal connections, `call_deferred`, Tween and RPC all look them up by name and pass Variant arguments. They must therefore have concrete signatures. A generic function must declare its type parameters explicitly, e.g. `func max[T: Ord](a: T, b: T) -> T`, and generic functions are not exposed to the engine.

Rationale:

- With implicit generalization, the static path and the `call()` dynamic path of the same call could produce different results. The static path instantiates with i32, while the dynamic path only has the 64-bit Variant int.
- If inference decided whether a function is generic, editing a function body could make a method disappear from `has_method`. A signal already connected in the editor would then fail to find its method at runtime.

### R3 Parameter types

A parameter's type is determined as follows:

1. If the method overrides an engine virtual function, the type comes from the engine contract.
2. Otherwise, look at the **positions with known types that the parameter flows directly into**: passed as an argument to a parameter of known type; assigned to a variable or member of known type; one operand of a binary operator whose other operand has a known type (the parameter then takes that same type); returned from a function with an annotated return type. If all such positions have the same type, that is the parameter's type.
3. Otherwise, an annotation is required.

Two things are not evidence:

- **Method calls.** `x.hit()` tells you nothing about x's type. A method with that name could belong to any class, and adding a new class should not change the meaning of existing code.
- **Flowing into an any or Option[any] position.** `print(x)` does not determine x's type as `any`; otherwise it would be a silent fallback to a dynamic type.

```
func take_damage(amount):     # hp is i32; hp -= amount → amount: i32
    hp -= amount
func _on_body_entered(body):  # only body.hit() is called → annotation required
    body.hit()
```

### R4 Local variables

A local variable's type is solved over the whole function body and then fixed.

- If the initializer has a concrete type, that type is final. Later assignments do not widen it; implicit conversions follow §7.3.
- The **open parts** of an initializer are flexible literals (§7.2) and the element type of an empty container. They are determined as follows:
  1. **Inflows first.** If values of known type flow into the variable (assignments, the right-hand side of compound assignments, elements written into an empty container), the type is the lossless common type of the initializer and all those values. For numbers, the common type follows §7.5, and if none exists an annotation is required. Non-numeric types must be identical; no common ancestor class is computed. An inflow made up only of the variable itself and flexible literals carries no type information.
  2. **Otherwise, outflows.** If there are no such inflows, look at the positions with known types that the variable flows into (the same kinds of positions as in R3). If they all agree, use that type. If they disagree, or there are none, a flexible literal takes its default type (i32 for integers, f64 for floats), and an empty container's element type requires an annotation.
  3. When two open locals are assigned to each other, they form a group that shares one type, determined from the inflows and outflows of the whole group.
  4. The result does not depend on the order in which the constraints appear.

```
var ratio = 0
ratio = position / length    # f64 flows in → ratio: f64; the literal 0 becomes f64

var t = 0.0
t += delta                   # f64 flows in → t: f64
sprite.modulate.a = t        # flows out to f32: precision loss, no warning by default

var groups = 1
dispatch(list, x, y, groups) # nothing flows in; all outflows are u32 → groups: u32
```

These rules are based on a survey of real Godot projects; see the [survey report](surveys/literal-local-constraints.en.md). The report records the warning defaults used in that survey; current defaults are specified in §7.3.

**Members are resolved in source order.** When you call a method or access a member on a value whose type is not yet determined, the constraints appearing before that point must determine the type under the rules above. Otherwise, it is an error asking for an annotation. If the value is a flexible literal with no constraints before that point, it takes its default type right there, and any later constraints become ordinary implicit conversions. This is the only case in R4 where the order of the code matters.

```
var a = 1
a.abs()             # no constraints before this point → a: i32
a = big_i64         # i64→i32: truncation; optional integer-conversion warning

var items = []
items[0].hp -= 1    # element type unknown here → error, even though append(enemy) follows
items.append(enemy)
```

### R5 Return types

The return type is inferred from every return in the function. When different branches return different types:

- different variants of the same enum merge into that enum;
- different numeric types merge into their common type per §7.5;
- different classes are not merged into a common ancestor. The first version reports an error and asks for an annotation, because the inferred ancestor is usually just Node or Object, and it would change whenever the class hierarchy changes;
- returning, say, an integer in one branch and a string in another does not produce an anonymous sum type.

### R6 Members and signals

A member variable's type comes from its annotation or its initializer. A member initialized with an empty container must be annotated; its type is not inferred from how other methods or other files use it, because the Inspector, `get_property_list` and inference in other files all need the member's type up front. Signal parameters must be annotated as well, because emit sites are callers (R1).

### R7 Inferred signatures are the interface

Editing a function body can change its inferred signature and make calls in other files fail. This is the inherent cost of an inference-first design. When it happens, the diagnostic must show three things: the type the caller expected, the type inferred for the callee, and the source line that determined it.

### 3.8 Where implicit conversions happen

Implicit numeric widening and conversion from a subclass to its superclass happen only at checking boundaries. These are assignments, arguments and return values whose expected type is known, and binary operators whose operands both already have concrete types. If a type is not yet determined, the check waits until it is.

## 4. Generics and traits

- A trait names a capability constraint explicitly. Whether a type satisfies a trait is decided structurally: a type with methods of matching signatures satisfies it, without having to declare so.
- Generic functions are the only source of polymorphism (R2). Inside a generic function, every method call and operator applied to a type parameter must be covered by a trait bound or a built-in operator constraint. Operators in generic code require both operands to have the same type (`T + T -> T`).
- In the first version, traits contain only method signatures and can be used only as bounds on generic functions. There are no default methods, associated types, generic traits or trait objects.
- Traits are not a back door for field injection, mixins or multiple inheritance, and they do not change the Node type tree.
- **Generic code must behave exactly like the same code written for a concrete type.** It must not fall back to Variant operations. For example, `add[T](a: T, b: T)` instantiated with i32 must follow 32-bit arithmetic and overflow rules, while a Variant int is 64-bit and would give different results. External types also copy and destroy differently: String is copy-on-write, and Ref is reference-counted.
- Implementation note (non-normative): the first version plans to pass a runtime type descriptor for each type parameter. A descriptor holds the type's size, its copy and destroy functions, and its trait method tables, so only one copy of the generic code is needed. "Static dispatch" here means that constraints resolve at compile time to a single implementation, which the runtime then calls indirectly through the descriptor. There is no lookup by name and there are no dyn objects. IR-level monomorphization and JIT specialization are possible later optimizations [Open].

## 5. Object model and lifetimes

### 5.1 Kinds of objects

Every class instance is a Godot Object with a ScriptInstance attached. A class without `extends` inherits RefCounted, as in GDScript, and the language's reference count is the engine's reference count. Semantics depend on the static type:

| Kind | Static type | Semantics |
|---|---|---|
| Owned reference | RefCounted and its subclasses (including Resource, and classes without `extends`) | A strong reference keeps the object alive; `weak` is available |
| Handle | Classes that do not derive from RefCounted (e.g. Node), except Object itself | A reference does not keep the object alive; it is essentially an ObjectID plus a liveness check. `weak` is meaningless and not allowed |
| Decided by the actual object | Object | A strong reference if the actual object is RefCounted, otherwise a handle; `weak` is available |
| Value | Numbers, bool, core value types such as Vector2, String, enums | Assignment copies (copy-on-write and similar optimizations come from the engine contract) |

**Changing the static type never drops ownership.** A RefCounted object upcast to Object is still held by a strong reference, and it stays that way when downcast back. This matches how Variant holds objects. Object is RefCounted's only ancestor, so Object is the only static type where the actual object decides the semantics. The same holds when generic code is instantiated with Object; the type descriptor takes care of it.

### 5.2 Reference cycles and Node lifetimes

- There is no tracing GC, and no move or borrow annotations.
- Reference cycles are not collected automatically. Break them with weak back-references, or manually at an explicit point in the object's lifetime. Do not count on destructors to break a cycle, because the cycle itself keeps the destructors from running.
- Node lifetimes are managed by the engine; references from the language never keep a Node alive.

### 5.3 External containers and write-back

- Array and Dictionary are shared references.
- A Packed*Array is shared by reference between script variables, but built-in engine properties and methods return copies of it. That is why `line.points.append(p)` silently does nothing in GDScript.
- **Write-back rule:** modifying a value-type obtained from a property performs "get, modify, set". This includes calling a method that modifies the value. For example, `node.position.x += 1` and `line.points.append(p)` both write the result back to the property. Whether a method modifies its receiver is determined from the builtin method's const information.

### 5.4 Copying and closures

- Copying must be explicit. Copying a reference, shallow-copying an object and deep-copying an object are different operations. External types offer whatever copy operations the engine gives them.
- Closures capture current values by value. A captured reference still points to the same object. A closure cannot rebind an outer local variable. Strong reference cycles created through closures must also be handled.
- [Open] Syntax for temporarily upgrading a weak reference; syntax for explicit copies and how they map to core's `duplicate`-style operations; special destruction orders and threading restrictions.

## 6. Enums, Option and nullability

### 6.1 Enums

- An enum has value semantics. Its representation at an engine boundary is specified in §12.4; storing it in `any` uses the typed boxing rule in §12.1. These encodings do not change its value semantics.
- **Enum values cannot be modified in place;** you construct a new value instead. Reference-type payloads are not deep-copied when an enum is copied.
- **[Known limitation] The first version does not support recursive enums.** An enum's value layout must not contain a cycle, including cycles through value types such as Option (e.g. `enum Tree: Node(i32, Tree?, Tree?)`). A cycle is a compile error. The diagnostic shows the cycle and suggests either using a class or recursing through Array or Dictionary. Recursion through those reference types is allowed, e.g. `enum Json: …, Arr(Array[Json]), Obj(Dictionary[String, Json])`. Godot has no lightweight box type, so every workaround goes through an Array or a RefCounted object. Demand is small, so the first version does not optimize for this.

### 6.2 `T?` is `Option[T]`

`Option[T]` has two variants: `None`, which represents absence, and `Some(T)`, which contains a value of T. `T?` is shorthand for `Option[T]`. There is no language-level `null` value.

Nesting is preserved. With `T = Enemy?`, `first[T](a: Array[T]) -> T?` distinguishes an empty array (`None`) from an array whose first element is absent (`Some(None)`). Neither value is `Some(Some(enemy))`.

Option has value semantics. Except for the representation guarantee below, its internal layout is unspecified; its engine encoding is specified in §12.4. A *niche* is a representation unavailable to values of the payload type that can instead represent an Option variant. Bare NIL is not a valid `any` representation, so `Option[any]` may use it for `None`, with `Some(v)` represented by v's non-NIL representation. `any` itself cannot use NIL to represent absence.

Putting `Option[T].None` into `any` produces a present value with dynamic type `Option[T]`, encoded in a non-NIL typed box (§12.1). It does not produce `Option[any].None`.

### 6.3 Narrowing

- After a check for `None`, the type is narrowed only for **local variables**. Closures capture by value (§5.4), so no other code can change a local between the check and its use. Narrowing locals is therefore sound.
- Members, properties and array elements are not narrowed. Any call between the check and the use could change them: a method call, a synchronous signal, a setter, or an await.
- There is a syntax that checks a value and binds it to a local in one step, e.g. `if var t = target:` (spelling [Open]).

### 6.4 Nullability sugar

- **Postfix `!`** extracts the value from `Some`. Applied to `None`, it is a program error, and the diagnostic gives the location. It guarantees presence, not that the object is still alive.
- **`?.`** is optional chaining. If the receiver is None, the whole chain short-circuits, and the result is nullable. `?.` is a single token; postfix `?` is reserved for Result propagation (§10). The language has no `?:` ternary operator, so there is no ambiguity.
- `?.` does not treat a freed object as None; it still reports an invalid-access error.

### 6.5 Non-null does not mean alive

- A Node reference can be non-null even though the Node has already been freed, so invalid-access checks always apply. For example, if a non-null local Node variable is held across an await, it is still non-null afterwards, but the Node may no longer be alive.
- When an engine contract is unclear, the value is treated as nullable until there is evidence, and the local binding descriptions are then filled in.
- Scanning an external mutable container once does not give a permanent static guarantee about its contents (see §12.2 for how containers are checked).
- [Open] Whether `$X` should include a non-null assertion (in GDScript, `$X` already reports an error when the node is missing). A uniform rule for which facts about external values can be narrowed reliably, and which calls invalidate them.

## 7. Numbers

### 7.1 Types

| Item | Rule |
|---|---|
| Signed integers | i8, i16, i32, i64 |
| Unsigned integers | u8, u16, u32, u64 |
| Floating point | f32, f64 |
| Float suffixes | `f` for f32, `d` for f64 |
| Integer suffixes | [Open] |
| i128, usize, etc. | Not provided |

### 7.2 Flexible literals

- An integer literal takes the type of the other operand or the expected type. If its value does not fit in that type, it is a compile error. With no constraint at all, it is i32.
- A float literal takes the expected type, or f64 if there is no constraint.
- An integer literal may also take a floating-point type, e.g. `var v: f32 = 10` or `Vector2(1, 2)`.

In `func f(x: u8): return x + 100`, the literal 100 becomes a u8 and so does the result. When x ≥ 156 the addition overflows, which is a program error (§7.4). In `Time.get_ticks_msec() - 1000`, 1000 becomes a u64.

### 7.3 Numeric conversions

Lossless numeric conversions happen automatically at checking boundaries (§3.8). Whether a conversion is lossless depends on the source type's full value range: i32→i64, u32→i64, i32→f64 and f32→f64 are lossless. Lossy implicit conversions are also permitted, subject to the constant rule below.

#### Integer conversions

For a non-constant integer value x converted to an N-bit integer type, let r be the unique integer with `0 ≤ r < 2^N` and `r ≡ x (mod 2^N)`. For an unsigned target, the result is r. For a signed target, the result is r if `r < 2^(N−1)`, and `r − 2^N` otherwise.

Thus narrowing discards high bits, and a same-width change of signedness preserves the bits. When widening, a signed source is sign-extended and an unsigned source is zero-extended, regardless of the target's signedness. These conversions have defined results even when they lose range. They are legal runtime operations and are independent of the arithmetic overflow setting.

| Source value and type | Target | Result |
|---|---|---|
| 300 as i32 | u8 | 44 |
| −1 as i8 | u8 | 255 |
| 255 as u8 | i8 | −1 |
| −1 as i8 | u16 | 65535 |
| 255 as u8 | i16 | 255 |

The table describes conversions of non-constant values. Converting a constant integer outside the target's range is a compile error, for implicit and explicit conversions alike. This error cannot be disabled. A constant within range is accepted unchanged.

```
const limit: i32 = 300
var small: u8 = limit         # compile error
var small_cast = u8(limit)    # compile error; cast spelling is tentative

func narrow(x: i32) -> u8:
    return x                 # defined conversion; 300 becomes 44 at runtime
```

The constant rule also applies when constant propagation establishes a single constant operand. A `var` declaration does not by itself prevent propagation. A range analysis showing only that a runtime value may lose range does not make that value a constant and cannot justify this error.

A compiler must diagnose out-of-range integer literals and out-of-range conversions in constant expressions. Further constant propagation may be incomplete in the first version and may improve in later versions, including across declarations or functions. An undiagnosed constant error remains an error; later diagnostics may reject it. Missing a diagnostic neither creates undefined behavior nor permits the optimizer to assume that the conversion is unreachable. If such a conversion executes, its result follows the integer conversion rule above.

#### Numeric values in any

Converting a numeric value held in `any` to a requested numeric type applies the numeric conversion rules of this section and §7.5 to its normalized value (§2.1). It does not require the requested type to match the original source width. A boxed u64 is first recovered as an unsigned u64 value, then converted to the requested type; its payload must not first be interpreted as a negative i64.

For example, storing an i32 value of 1 or an i64 value of 1 produces the same canonical i64 value in `any`. In the following example x is a runtime input; if its value is 18446744073709551615, same has that value and signed is −1 under the integer conversion rule:

```
func convert_number(x: u64):
    var value: any = x
    var same: u64 = u64(value)
    var signed: i64 = i64(value)
```

The explicit conversion spelling is tentative. This example describes runtime conversions, not a constant out-of-range conversion. Storing a value in `any` does not exempt it from the constant rule when constant propagation establishes the operand's value. A non-numeric value does not become numeric merely because a numeric target is requested.

#### Conversion warnings

| Warning class | Covers | Default |
|---|---|---|
| Precision loss | f64→f32; integer→float when precision may be lost (e.g. i32→f32, i64→f64) | Off |
| Integer conversion loss | Integer narrowing and changes of signedness that may lose range | Off |
| Float-to-integer range loss | Float→integer | On |

Each class can be set independently to off, warn or error. These settings do not disable constant errors. A constant exactly representable in the target type produces no loss warning. An explicit conversion accepts the corresponding loss, but does not exempt a constant from the integer range rule.

The engine mixes f32 and f64: `_process(delta)` receives a double, while Vector2 components are real_t (float in standard builds; Appendix A). With default settings, `position += velocity * delta` produces no warnings. Float conversions retain the rules in §7.5.

### 7.4 Overflow

- **Integer overflow is a program error**, in the same category as out-of-bounds access and division by zero.
- Runtime overflow detection is off by default. When it is off, the result of an overflow is well-defined: it wraps around at the bit width (two's complement). It is **not undefined behavior**. Optimizations must not assume overflow never happens, and the interpreter and the JIT must produce identical results. Lockstep multiplayer and replays depend on this determinism.
- Even though the result is well-defined, overflow is still an error. The same program reports it when detection is on, and it is a compile error whenever the overflow can be proven.
- When wrapping is what you want, use **explicit wrapping operations**, e.g. `wrapping_add` or `&+` (spelling [Open]). They never report errors, whether detection is on or off. Hashes, random number generators, and tricks like `mask -= 1` to get all ones should use them.
- **Every overflow that can be proven at compile time is an error.** An expression that overflows every time it is evaluated is a compile error.
  - Every implementation must catch at least two cases: overflow while evaluating a constant expression, and a literal that does not fit its target type.
  - Detection of further cases is added over time. Each newly detected kind of case gets a conformance test, and detection is never removed. If a compiler upgrade makes old code fail to compile, that code was already wrong.
  - Proofs only look inside a single function. An overflow that can only be shown by reasoning across functions is at most a warning, never a compile error, so incremental compilation and error locations stay stable.
  - No false positives: an error is reported only when the expression overflows on every evaluation. Overflows that merely might happen belong to potential-risk warnings. The conditions for those are [Open], and the first version may issue none.
  - Within one version, editor diagnostics and export builds must use the same analyzer.

### 7.5 Mixed arithmetic, division and edge cases

**Mixed arithmetic:** the result has the common type that can hold both operands losslessly, e.g. i32+i64→i64, i32+u32→i64, f32+f64→f64. When no such type exists (e.g. i64+u64), an explicit conversion is required. How integer and float operands combine is [Open].

| Case | Rule |
|---|---|
| Integer `/` | Truncates toward zero; there is no `//` operator |
| Division by zero (`/` and `%`) | Always a program error, regardless of the overflow detection setting |
| Sign of `%` | Same as the dividend |
| MIN / -1 | Counts as overflow: an error with detection on, MIN with it off. MIN % -1 is always 0 |
| Shift count beyond the bit width | A program error: reported with detection on; with it off, the count is taken modulo the bit width |
| Float→integer (explicit or implicit) | Out-of-range values saturate to the target's minimum or maximum; NaN becomes 0 |

In C++, an out-of-range float→integer `static_cast` is undefined behavior; the language does not copy that. The scope of diagnostics for float rounding and float overflow is [Open].

### 7.6 Engine boundary

- GDScript's default scalar widths are not inherited. Three notions of type are kept apart: the type in the language, the type in the engine API, and the ABI slot that Variant/ptrcall actually uses.
- The engine API types come from `MethodBind::get_argument_meta()`, which returns a `GodotTypeInfo::Metadata` value such as INT_IS_UINT64. extension_api_dump uses the same data to produce its `meta` field, so no hand-written table of method types is needed.
- For a statically known u64 engine API slot, the ABI encoding between u64 and Variant int reinterprets the bits (§12.3); it is not a source-level constant conversion under §7.3. Storing u64 in `any` instead uses a typed box (§12.1). A real-world test case: the ObjectID of a RefCounted object has bit 63 set, so as a u64 it is at least 2⁶³, and it becomes negative when passed through a Variant.
- Runtime bounds checking is separate from overflow detection and is on by default. [Known limitation] The first version offers no way to turn bounds checking off and has no unsafe mode.

## 8. Variables, constants and initialization

### 8.1 var and const

- The keywords are `var` and `const`; there is no separate `let` or `mut`.
- `const` accepts only constant expressions. There are no constants that are computed at runtime and then frozen, no `@onready const`, and no freezing phase.
- Members, construction, scene injection and `@onready` all follow Godot's lifecycle. The language adds no user-facing types for tracking initialization state.
- A non-null member must be initialized from a valid source; `None` is not a value of T. Use `T?` for a value that may be absent.
- A member with deferred initialization may have an internal "uninitialized" state. Reading it while uninitialized is a compile error when this can be decided statically; otherwise it is checked at runtime and reported with a clear diagnostic.

### 8.2 Initialization order and rules

The language uses GDScript's initialization order:

- At construction, the initializers of all ordinary members run first, from base class to derived class, and then `_init` is called.
- `@onready` initializers run when `_ready` is called, before the body of the user's `_ready`, also from base class to derived class.

On top of that order:

1. **Construction-time member initializers must not reference self.** They may not call instance methods or read other non-const members. This is checked statically. It rules out a whole class of bugs where a base-class initializer makes a virtual call that reads a derived-class member that has not been initialized yet. **`@onready` initializers are exempt:** they run once the node is ready and may reference self, as in the common `@onready var child = $Child`.
2. **The `_init` of a Node or Resource subclass must be callable with no arguments.** Scene instantiation, resource loading and `duplicate()` all call `_init` without arguments. A violation is reported at compile time, or at instantiation time.
3. **Each non-null `@onready` member has its own "initialized" flag.** The check is whether that member's initializer has finished, not whether `_ready` has started. Reading a member before its initialization has finished is a program error, and the diagnostic names the member and shows the call stack. This cannot be ruled out statically in general. Two examples:
   - A child's `_ready` runs before its parent's. If the child synchronously emits a signal, and the parent's handler reads one of the parent's `@onready` members, that member is not initialized yet.
   - An `@onready` initializer calls a method that reads another `@onready` member declared later, which is not initialized yet. If the initializer reads a later-declared member directly, this is reported at compile time.

### 8.3 Open questions

- When a non-null `@export` reference is checked: once scene instantiation completes, or on first read.
- The initialization order of static vars.
- The complete order of member initialization, `_init`, scene properties, entering the tree, `@onready` and `_ready` for the chosen target version, especially with inheritance and scene injection.

## 9. await, signals and Callable

- await is integrated minimally with Godot's signals, timers, frame events and main loop. There is no separate executor. The language is responsible for suspending a function's frame and for saving and restoring its local variables.
- Lifetimes, resumption order and reentrancy follow GDScript as closely as possible. The language does not add its own automatic deferral or cancellation mechanism. A signal can resume a suspended function immediately, so code must not assume it continues only on the next frame.
- When a suspended function resumes, the instance it belongs to is checked. If the instance no longer exists, an error is reported and the function is not resumed. This follows GDScript, which in release builds returns silently instead. The claim that an await is cancelled when its node leaves the tree has not been verified and must not be treated as a rule.
- Functions, closures and callbacks whose signatures are known use static function types, so their arguments and results are checked at compile time.
- Engine Callables whose signatures are unknown keep a type-erased form. In the first version they use callv through the binding layer. Arguments use `any` or `Option[any]` according to the binding contract; an unknown result is `Option[any]` unless a reliable contract excludes NIL (§12.6). A concrete operation requires narrowing.
- [Open] The following cases must be tested against the target version: the node's owner being destroyed, the node leaving the tree, the signal source being destroyed, the suspended frame being freed, and async values that cross language boundaries. Double resumption, leaks and invalid access must all be avoided.
- [Later] Variadic generics that keep full signatures (parameter packs). These are not a reason to adopt full row polymorphism.

## 10. Errors and diagnostics

- Failures that are expected and can be handled are returned as Result values and passed upward with a propagation syntax. Postfix `?` is reserved as the propagation token; its exact semantics are [Open].
- **Program errors** are: out-of-bounds access, invalid access, reading an uninitialized value, division by zero, a failed non-null assertion, integer overflow with detection on, and a failed external-value check (§12.5–§12.6). A program error produces a runtime diagnostic and a call stack. Where script execution stops is coordinated with the engine debugger; the whole game does not have to exit.
- **Error messages are a priority, even if they look rough in the first version.** What matters first is that the file, the location and the chain of constraints that led to the error are correct.
- An error in another file must not be reported only as "the entry script failed to load". A type conflict shows the types on both sides and the relevant source locations (R7).
- If compilation fails, no partially compiled signatures or executables are published. Polished formatting, fix-it suggestions and full inference traces can come later.

## 11. Compilation model

- Recursion, mutual recursion and inference across files are solved jointly for each strongly connected component (SCC). Signatures exported across files may be inferred; annotating every public function is not required.
- **SCCs are formed per declaration** (function or member), not per file. Files in Godot projects reference each other all the time (Player, Enemy, autoloads), so grouping by file would put most of a project into one SCC.
- **Dependencies are discovered on demand, during inference.** The dependency graph cannot be computed before inference, because the target of a method call depends on the receiver's type. In `func f(): return make_b().g()`, the dependency on `B.g` only becomes visible after inferring that `make_b` returns B. The procedure:
  - while inferring one declaration, request the signatures of other declarations as they are needed;
  - if a request reaches a declaration that is still being inferred, that is a back edge, and the declarations involved are merged into one SCC using Tarjan's stack;
  - signatures are published only once the root of the SCC has finished.
- Reentrancy is handled by marking declarations as "being inferred" and by the stack. Until an SCC completes, its signatures are visible only within the stack. If inference fails, the whole SCC is discarded, so no half-finished results are cached.
- Polymorphic recursion is not supported.
- The module has its own coordinator, so the language does not have to infer one file at a time just because the engine loads and validates resources one at a time. The language does not require a joint-compilation interface from the engine, and core is not modified for this.
- **Initialization cycles are a separate problem from type cycles.** const values are constant expressions, so a cycle among them is rejected. Construction-time member initializers cannot reference self (§8.2). The only remaining case is objects constructing each other, e.g. A has a member initialized with `B.new()` and B has a member initialized with `A.new()`. That is a runtime stack overflow and has nothing to do with inference.
- Incremental compilation tracks dependencies, reverse dependencies and caches. Correct invalidation comes first: recompiling more than necessary is acceptable, and the first version may simply recompile the whole project. When a declaration's interface has not changed, its callers do not need to be recompiled. Incremental compilation is not hot reload, and ordinary runtime calls never trigger inference.

## 12. Engine interop: the Variant mapping

### 12.1 Values, representations and ABI

Godot Variant is the engine's tagged value container. It is an interoperation concept, not a second dynamic language type. Language code uses `any` and `Option[any]` (§2.1); the binding layer adapts them automatically to engine Variant slots.

The internal representation of `any` is compatible with the Godot Variant ABI, including its value layout and handling at Variant call boundaries. Numeric values use the canonical representations below. Other core-representable values use their native representations. Values without a native representation, including language enums and Option values stored in `any`, use read-only Arrays carrying concrete type information and their payload. This type information includes the type arguments needed to recover the value. The exact typed-box tag format is [Open].

| Numeric source type | Dynamic numeric type in any | Representation |
|---|---|---|
| i8…i64, u8…u32 | i64 | Native Variant INT, preserving the numeric value |
| f32, f64 | f64 | Native Variant FLOAT, preserving the numeric value |
| u64 | u64 | Typed Array box preserving the full unsigned value, regardless of its magnitude |

Recovering a number from `any` uses the requested target type and §7.3/§7.5, rather than testing for the source width. Recovering a boxed u64 restores its unsigned value before any numeric conversion. A u64 box is not silently turned into an ordinary signed Variant INT. For a statically known u64 engine API target, the binding may use the separate bit-transport contract in §12.3; an untyped Variant slot receives the dynamic box.

A typed box is non-NIL even when its payload represents `Option[T].None`. Below, `box(T, e)` denotes a non-NIL read-only Array carrying type T and payload encoding e. This is schematic notation, not source syntax or a fixed tag format. For example, `box(Option[i32], NIL)` encodes a present `any` value containing `Option[i32].None`. `Option[any].None`, by contrast, is represented by NIL, and `Some(v)` by v's non-NIL representation. Bare NIL is never a valid `any`.

The encoding of a statically known T at an engine boundary (§12.3–§12.4) need not be the encoding of that value stored in `any`. In particular, enum encoding may use NIL or omit type arguments when the static type supplies them; dynamic boxing must retain the type information and must not produce bare NIL. A shared ABI does not make these value domains identical.

GDScript, the editor, Dictionary keys and resource files can observe external encodings. The rules below specify those encodings by type; they are not selected anew for each value. A later change to an established external encoding is a breaking change.

### 12.2 Containers

- **The language's containers are the engine's containers.** `Array[T]` and `Dictionary[K, V]` are the engine's Array and Dictionary, shared with the engine rather than copied. When the engine can express the element type, the engine's typed container is used.
- The type guarantee has two parts. Writes from the language are checked at compile time, and **every read is checked at runtime**. Reading a value that does not match T is a program error, with a diagnostic such as "element 3 is null, but the element type is Enemy (possibly written by external code)". An implementation may skip the read check when it can prove that only the language ever wrote to the container. That is an optimization and does not change the semantics.
- Rationale: the engine side can write anything into a container. The engine's typed Array allows null for Object elements, and an `Array[i32]` can only be an `Array[int]` on the engine side. If the language used its own containers and copied them at the boundary, it would not see changes the engine makes, which breaks the guarantee that an Array is a shared reference.
- Element types without a default value, such as non-null Object types, offer `resize(n, fill)` but not `resize(n)`. The engine's `Array::resize` leaves new Object elements as null.

### 12.3 Type mapping table

This table specifies external representations for declared target types. The u64 row is the bit transport for a statically known u64 target; a u64 stored in `any` follows §12.1 instead. The read checks validate an external representation against its target type; they are distinct from numeric conversion of an already valid language `any` (§7.3).

| Language type | Representation in Variant | Check when read back |
|---|---|---|
| i8…i64, u8…u32 | int | Value is in range |
| u64 | int, bits reinterpreted | None (values ≥2⁶³ appear negative, as with the engine's ObjectID) |
| f32, f64 | float | f32 is rounded; this is a precision loss |
| Core value types (Vector2, String, etc.) | The type of the same name | Type matches |
| Non-null Object class T | Object | Non-null and the class matches |
| enum (including Option) | See §12.4 | Fully decoded; see §12.5 |
| `any` | Canonical i64/f64 for ordinary numbers; boxed u64; other native non-NIL values or typed boxes (§12.1) | Reject NIL; validate typed boxes and their payloads; numeric extraction follows §7.3 |
| `Option[any]` | NIL for None; any's non-NIL representation for Some | NIL yields None; otherwise check as any |
| Function types | Callable | Signature is verified for Callables the language created; others are checked on every call |
| `Array[T]`, `Dictionary[K, V]` | Array, Dictionary | Each element is checked when read |

### 12.4 Enum mapping

This section encodes a statically known enum type for the engine. Storing an enum value in `any` follows §12.1 instead.

**Representation set:** the set of Variant type tags used by a type's external encoding. Integers and core value types each use a single tag. A non-null Object class uses {OBJECT}. `any` uses all non-NIL tags, and `Option[any]` also includes NIL. An enum's set is the union of its variants' sets after selecting the encoding in its enclosing context; a fully boxed enum uses {ARRAY}. These sets describe the selected encoding; they do not determine which enclosing layer must be boxed.

**Rules:** encoding is selected from the outer enum inward, by type and payload position, not by the particular value.

1. **Variants without payload:** initially reserve NIL if there is exactly one; if there are several, each is represented by its index as an INT. An enum with several variants and no payloads therefore uses int indices, like a GDScript enum.
2. **Single-field variants:** initially use the field's encoding. If the outer enum has reserved NIL for its unique payload-free variant and the field's standalone encoding includes NIL, encode the inner field value in a non-NIL `box(T, e)`, where T is the field type and e its encoding (§12.1). This choice applies to every value in that payload position. The outer payload-free variant keeps NIL unless the remaining-conflict rule below requires full boxing.
3. **Multi-field variants:** boxed as a read-only Array `[&"Enum.Variant", field1, field2, …]`. The leading StringName records the enum name and the variant name, without type arguments: both `Option[i32]` and `Option[String]` use `&"Option.Some"`, and the static type at the point of reading supplies the type arguments. Each field uses its own encoding recursively. An `any` field follows §12.1.
4. **Remaining conflicts:** check the representation sets again after selecting the payload encodings, including every ARRAY introduced by an inner box. If any variants still overlap, all variants of the outer enum are boxed, including payload-free variants (`[&"Enum.Variant"]`) and single-field variants (`[&"Enum.Variant", field]`). Their explicit variant tags make decoding unique; inside these variant-tagged boxes, fields use their types' standalone encodings recursively, as in rule 3. Several payload-free variants sharing distinct INT indices do not count as a conflict. No additional layout optimization is applied to avoid this full-boxing rule.
5. Object payloads are told apart only by their Variant type (OBJECT), not by class, because subclasses overlap.

```
enum Cmd: Stop, Move(Vector3), Wait(f64), Hit(Node, i32)
# Stop→NIL  Move→Vector3  Wait→float  Hit→[&"Cmd.Hit", node, 5]

enum Hp: Dead, Alive(i32)              # Dead→NIL  Alive→int
enum Shape: Circle(f64), Square(f64)   # both float: conflict → all boxed
enum Key: A, B, Code(i32)              # index and Code both int: conflict → all boxed
```

```
enum E: Empty, Has(Option[i32])
# Empty        → NIL
# Has(None)    → box(Option[i32], NIL)
# Has(Some(5)) → box(Option[i32], 5)
```

If another variant uses ARRAY, inner boxing may create a remaining conflict. For example, `enum F: Empty, Has(Option[i32]), Items(Array[i32])` has both Has and Items represented as ARRAY after step 2; step 4 therefore boxes every F variant, including Empty.

**Option is an instance of these rules.** Its sole payload-free variant None reserves NIL. For `Some(x)`, a T whose standalone encoding excludes NIL is encoded directly; otherwise the inner T value uses `box(T, e)`. These payload choices do not conflict with the outer NIL, so None retains the NIL niche. No Option-specific conflict rule is needed.

An Option has `None` and `Some` values, not a language-level null value. NIL in its standalone encoding does not prevent an enclosing Option from reserving NIL for its own `None`. `any` and language Object types are non-null; `Option[any]` and `Option[Enemy]` therefore need no payload box solely for absence. Raw engine null is handled by the binding and read checks (§12.5–§12.6).

| Type | Representation set | `None` | `Some(…)` |
|---|---|---|---|
| `i32?` | {NIL, INT} | NIL | `Some(5)` → `5` |
| `Enemy?` | {NIL, OBJECT} | NIL | the object itself |
| `i32??` | {NIL, ARRAY} | NIL | `Some(None)` → `box(Option[i32], NIL)`; `Some(Some(5))` → `box(Option[i32], 5)` |
| `Cmd?` (Cmd above) | {NIL, ARRAY} | NIL | `Some(Cmd.Stop)` → `box(Cmd, NIL)`; `Some(Cmd.Move(v))` → `box(Cmd, v)` |
| `any?` | all tags | NIL | `Some(v)` → v's non-NIL representation, including `box(Option[i32], NIL)` when v contains `Option[i32].None` |
| `any??` | {NIL, ARRAY} | NIL | `Some(None)` → `box(Option[any], NIL)`; `Some(Some(v))` → `box(Option[any], v's representation)` |

NIL in these examples denotes an engine encoding, not a language value. For example, the `i32???` value `Some(Some(None))` is encoded as `box(Option[Option[i32]], box(Option[i32], NIL))`. Nested boxes preserve the Option layers. An Option stored in `any` likewise retains its complete type in a non-NIL typed box (§12.1); this does not turn it into the enclosing `Option[any].None`. The niche of an enclosing Option and the dynamic boxing of a value into `any` serve separate purposes. Distinct levels of `None` must not be collapsed.

**Boxes and identity:**

Boxes are read-only Arrays. Objects compare by identity, both with `==` and as Dictionary keys; Arrays compare and hash by content and can be serialized directly (Appendix A). Enum values are immutable, so sharing a box preserves their value semantics. No bit packing is used: a Variant has a single type tag, and copying it preserves only the concrete type's value, not spare bytes in its data area (Appendix A).

An enum inlined directly into an engine Variant loses its type identity. For example, `Cmd.Move(v)` becomes a Vector3, so decoding can check its shape but cannot recover its origin; in an untyped Dictionary it is the same key as v. A typed box in `any` retains the enum's concrete type. These are different encodings (§12.1).

Payload-free enums encoded by index change their saved meaning if variants are reordered; named boxes change if a variant is renamed.

**Depth:** encoding that would exceed the engine's recursion limit (`MAX_RECURSION`, currently 100; Appendix A) is a program error. Decoding enforces the same limit. This guarantees that the language's encoder does not construct an over-depth encoding. It does not guarantee that external code cannot later deepen a shared Array or Dictionary. Normal checks on subsequent reads and object accesses still apply (§12.2, §12.5); the construction guarantee does not validate future external contents.

### 12.5 Reading back and decoding

An external mutable location is checked whenever its contents enter the language as a value of a trusted type. A successful check of a location does not validate that address permanently. Re-reading a property or container element requires checking its current value.

For an `any` read, NIL is a program error. For an `Option[any]` read, NIL is `None`, and a valid non-NIL value is `Some(v)`. Typed boxes must have valid type information and a payload matching that type. Other target types are checked as follows:

- Inlined enums: the Variant tag must identify a unique variant under the encoding selected in §12.4. NIL identifies a payload-free variant when NIL is reserved for it. A single-field variant is decoded using its selected payload encoding: validate and unwrap a required typed box before decoding its payload as the field type, or decode directly when no box is required. The field's range, presence and class requirements still apply. This includes Option without a separate decoding rule.
- Variant-tagged enum boxes: the Array's leading StringName must name a variant of the expected enum, its length must be one plus that variant's field count, and every field must match its type. Nested enums are decoded recursively.
- Other types: the checks in §12.3 apply.

Decoding an enum copies its fields into a language value without retaining the external encoding box. Changing that box's variant tag or value fields afterwards does not change the decoded enum. Reference-type payloads remain shared. In particular, copying a Variant isolates the value slot, not the contents of an Array or Dictionary it refers to. A checked `any` copy therefore does not certify shared container elements; each read follows §12.2. If a typed box remains externally mutable, its type information and payload must be checked again when read or decoded; copying its Variant slot alone does not make that box trusted. Non-null Object values still require the liveness checks in §6.5.

The language emits read-only boxes and may reuse them when that preserves value semantics. Receiving an external box does not establish that its shared contents will remain unchanged.

### 12.6 Binding contracts and entry conversion

An engine API Variant maps to `any` only when a reliable binding contract excludes NIL. Otherwise it maps to `Option[any]`. This applies to arguments, return values and properties. The binding layer performs the encoding and checking automatically; ordinary calls do not require source-level conversion to a raw Variant type.

Instances of the language's classes are Godot Objects. GDScript and engine code can therefore bypass static types, for example with `node.call("take_damage", "hello")`, `node.set("hp", 1 << 40)`, scene property values, AnimationPlayer, Tween or clearing an exported reference in the Inspector.

Every incoming Variant is decoded and checked against the declared target type (§12.3, §12.5). This includes callp arguments, `set`, signal callbacks, scene deserialization and Callable calls. A wrong type, an out-of-range integer or absence in a non-null position is a runtime error. These are checks on external representations, not the integer-to-integer conversions of language values in §7.3; they do not silently truncate. An external Variant INT of 300 read directly as u8 is an error. Converting a valid language `any` holding a runtime canonical i64 value of 300 to u8 yields 44. A direct external read must not be routed through an implicit `any` numeric conversion to bypass the target type's range or type checks.

If external code writes NIL to a location exposed as `Option[any]`, the next read yields `None`. If the location is exposed as `any`, the next read fails the check. The same rules apply when external code modifies an aliased container.

### 12.7 Open questions

- Which types have a default value, for example whether an enum's default is one of its variants. This decides whether `resize(n)` is available.
- Whether narrow integer `@export` types such as i32 should get an automatic range hint, since the Inspector edits a 64-bit int.
- How the debugger should decode and display inlined enums and boxed values according to their static type.

## 13. Modules and engine facilities

The language keeps Godot's existing conventions for script files, resource paths and class registration. There is no new package manager and no elaborate visibility system. Exported properties, signals, resources, scenes and the editor reuse existing mechanisms wherever possible, but reuse alone does not prove compatibility; each still has to be verified. The first version does not specifically extend `@tool` or other ways of running scripts in the editor. These are supported if they work naturally, and otherwise deferred.

## 14. First-version scope

The first version includes `any`, `Option[any]`, typed dynamic boxes and external read checks (§2.1, §12). Incomplete constant propagation does not relax the constant-error rule (§7.3).

**[Known limitation]**

- No dyn traits. Code that treats objects of different types alike, such as iterating `get_overlapping_bodies()` and calling `take_damage` on each, must use `is` narrowing or a type-erased `call()`.
- No recursive enums (§6.1).
- Generic functions are not exposed to the engine (R2).
- No way to turn off bounds checking, and no unsafe mode (§7.6).
- No hot reload and no migration of live state.

**[Open]**

- The target Godot version. The suggestion is to track master. Tests for initialization order, await, typed Array and similar behavior will be pinned to whichever version is chosen.
- The implementation language, IR, JIT backend, distribution, and how deeply to integrate with the editor.
- Concrete syntax for enums, traits, generics, Result propagation, binding, explicit conversions, explicit wrapping operations and integer suffixes.
- Whether a `const` without a type annotation is flexible like a literal (like Go's untyped constants), and what type engine enum and bit-flag constants map to.
- How integer and float operands combine in mixed arithmetic; which float rounding and float overflow cases get diagnostics; when to warn about potential overflow.
- How to implement generics: by passing type descriptors, or by monomorphizing at the IR level (§4).
- The details listed in §5.4, §6.5, §8.3, §9 and §12.7.

**[Later]**

- **struct (value-type records)**: community proposals for this already exist. A struct maps to Variant the same way as an enum with a single variant: a single-field struct is represented as that field, and a multi-field struct is boxed as `[&"Name", …]`. Struct fields are mutable, so modifying a struct stored in a container goes through "get, modify, set". One risk: if core later adds a native struct Variant type, moving to it will change the save format and require a versioned migration.
- **A lint for boxing costs**, flagging allocations caused by boxing enums and structs. These mostly occur in containers such as `Array[enum]`.
- **Recursive enums**: if real demand appears, the preferred design is an explicit Swift-style `indirect`. It makes the allocation visible in the declaration, and it can later be relaxed to automatic indirection without breaking code. Automatic indirection would save only one keyword on a few types, while hiding heap allocations inside enums.
- dyn traits, default trait methods and associated types; parameter packs that keep full signatures; allowing implicit generalization for functions the engine cannot see (a compatible relaxation of R2).
- JIT and generic specialization; `@tool`; contributing contract metadata upstream to Godot.

## 15. Conformance test points

| Area | Minimum verification |
|---|---|
| Inference | Parameter types inferred from outflows; annotation required when only method calls are present; flowing into any or Option[any] is not evidence; for locals, inflows first and outflows second, with results independent of the order of constraints; flexible literals fixed to their default type at a method call; empty-container members require annotation; branches returning different classes are not merged into a common ancestor |
| Generics | Explicit generics instantiate; generic methods are invisible to `has_method`; instantiating with i32 gives 32-bit semantics |
| SCC | Mutual recursion resolves within a file and across files (including the `make_b().g()` example in §11); generic utility functions outside the cycle are unaffected; conflicts are reported at their real source |
| Enums | Payloads, named returns and exhaustiveness checks; no anonymous sum types are synthesized; enum values cannot be modified in place; a recursive enum is an error that shows the cycle, while recursion through Array, Dictionary or a class compiles |
| Traits | Compile results when a constraint is satisfied and when it is not, without relying on dyn |
| Nullability | `T?` and `Option[T]` are equivalent; `T??` distinguishes its two levels; the diagnostic when `!` fails; `?.` short-circuits the whole chain; members are not narrowed |
| Liveness | A local Node held across an await is still non-null but may no longer be alive; `?.` on a freed object reports invalid access |
| Ownership | A RefCounted object held only by an Object-typed variable stays alive; ownership is unchanged after downcasting; `weak` on a handle type is an error |
| Sharing and write-back | Changes made through one alias are visible through the others; `line.points.append(p)` writes back to the property; explicit copies never secretly deep-copy; weak back-references and manual cycle breaking work |
| Numbers | `position += velocity * delta` produces no warnings by default; warning classes are independently configurable; integer conversion loss defaults to off, float-to-integer range loss remains on; with `x: u8`, `x + 100` has type u8; `var a: u8 = 300` is a compile error |
| Integer conversions | Narrowing, same-width sign changes, signed and unsigned widening, including negative signed to wider unsigned; all results follow §7.3; in-range constants are accepted, out-of-range constant conversions are errors even with an explicit cast or warnings off; propagation is not stopped by `var`; a runtime range that may lose information is not a constant error |
| Overflow | Overflow in a constant expression is a compile error; explicit wrapping operations never report errors; turning runtime detection on and off works; the interpreter and JIT agree with detection off; an overflow that can only be shown across functions is not a compile error |
| Numeric edge cases | Every row of the table in §7.5; u64 round trip (bit 63 of ObjectID); bounds checking is unaffected by the overflow detection setting |
| Initialization | A construction-time member initializer that references self is an error, while `@onready` initializers may reference self; an `@onready` initializer that reaches a later-declared member through a method call is caught; `_init` is checked to be callable with no arguments; a child's `_ready` reading a parent's `@onready` member through a signal produces a readable diagnostic |
| await | Resumption order, reentrancy, resuming after the instance is destroyed, and frame cleanup, all compared against the target version |
| Callable | Known signatures are checked statically; type-erased results never pose as a known type |
| Dynamic values | Ordinary integers normalize to i64 and floats to f64 without preserving source width; u64 always uses a typed box and is recovered unsigned before target conversion; a runtime maximum u64 round-trips to u64 and converts to i64 as −1; basic and user-defined values, including Option.None, enter any; Object inheritance is unchanged; bare NIL is rejected for any and becomes None for Option[any]; a typed Option.None box remains present in any; nested Option boxes preserve every layer |
| Variant mapping | Every table row round-trips; a unique payload-free variant reserves NIL and a conflicting inner payload is boxed; remaining conflicts, including ARRAY collisions caused by that box, box every outer variant; §12.4's nested Options round-trip with outer None encoded as NIL, inner Option values boxed, and every None layer distinguished; boxes used as Dictionary keys compare by content; encoding and decoding enforce depth limits; decoding checks type information, variant, length and fields |
| External reads and entry conversion | Wrong types, out-of-range external integers and missing non-null values report errors; Array::resize-produced nulls are checked on read; an external NIL write becomes None or an error according to the target; copied Variant slots do not deep-copy Array/Dictionary contents; later external changes are checked on read, not covered by the encoder's construction guarantee; decoded enum value fields remain independent of their encoding box |
| Incremental compilation | After a change to a function body, a signature or an SCC it depends on, no stale results are reused |
| Engine integration | End-to-end: attaching scripts, callbacks, properties, saving and loading scenes |
| Diagnostics | Correct file and location, both types and the line that determined the type, a runtime call stack; no reliance on hot reload |

Code completion, breakpoints, exporting executables and similar features are accepted in tiers according to their cost; they are not all mandatory for an alpha.

## 16. Roadmap and feedback

**Roadmap**

1. Collect public feedback and freeze a minimal core that can be tested.
2. Build the test suite. Take stock of existing tests that could be reused and of their licenses. Separate behavior inherited from GDScript, behavior deliberately changed, and new capabilities. Cover positive, negative, runtime and engine-integration cases.
3. Implement the out-of-tree module, using the tests to verify the converged specification. Do not add features first.
4. Use AI assistance to port a mid-sized game project, roughly the size and shape of something like Brotato. This tests expressiveness, engine compatibility, diagnostics and development speed. Only use source code and assets that are legally available and licensed for this; if none are, choose a licensable project of similar size. Record:
   - how much manual annotation and adaptation the port needed,
   - ambiguities in the language,
   - engine contracts that were missing,
   - behavior that differs from the original,
   - performance bottlenecks,
   - debugging costs.

   Do not quietly loosen types or delete valid tests just to get the port to pass.

**Feedback**

Feedback is welcome as GitHub issues. A useful format is **problem → minimal counterexample → section → severity → minimal revision → test**. Please also say which of these it is: blocks finalization, needs refinement before implementation, or a later optimization. We are especially interested in:

1. Is the mapping table in §12.3 complete? Do StringName, NodePath, RID, Signal or other types need their own rows?
2. Are R3 and R4 too strict or too loose for real code? How many annotations would they require?
3. Is the guaranteed minimum for compile-time overflow detection in §7.4 precise enough to write tests for?
4. Entry conversion in §12.6, and when non-null `@export` values should be checked.
5. Do the initialization and await rules match the actual behavior of the chosen target version?

## Appendix A: Engine facts

Checked against Godot master (4.8-beta, commit `469659e`). Paths are relative to the root of the Godot repository.

| Fact | Source |
|---|---|
| The parameter of `_process` and `_physics_process` is a double | `scene/main/node.h:433-434` |
| `real_t` is float in standard builds | `core/math/math_defs.h:146` |
| `Time.get_ticks_msec()` returns uint64_t | `core/os/time.h:77` |
| Bit 63 of an ObjectID marks RefCounted objects | `core/object/object_id.h:45` |
| RefCounted derives directly from Object | `core/object/ref_counted.h:36` |
| `Array::resize` does not initialize new Object elements | `core/variant/array.cpp:313-325` |
| The engine's typed Array accepts null for Object elements | `core/variant/container_type_validate.h:60` |
| Packed arrays are passed by reference between scripts, but built-in properties and methods return copies | `doc/classes/PackedInt32Array.xml:9` |
| GDScript runs all member initializers, from base class to derived class, before calling `_init` | `modules/gdscript/gdscript.cpp:144, 176, 194` |
| GDScript runs `@onready` initializers, from base class to derived class, when `_ready` is called and before the user's `_ready` body | `modules/gdscript/gdscript.cpp:1956-1972` |
| When an await resumes, GDScript checks that the script and the instance still exist | `modules/gdscript/gdscript_function.cpp:274-287` |
| MethodBind provides metadata for each argument | `core/object/method_bind.h:102` |
| A Variant is a type tag plus a 16-byte data area (larger in double-precision builds); Transform2D, AABB, Basis, Transform3D and Projection are stored on the heap | `core/variant/variant.h:168, 259-266` |
| Copying a Variant copies only the value of its concrete type | `core/variant/variant.cpp:1187` onward |
| An Array is an ArrayPrivate plus an element buffer; making it read-only allocates an additional Variant | `core/variant/array.cpp:45-49, 958-962` |
| Creating an Object registers it in ObjectDB while holding a global spin lock | `core/object/object.cpp:2370, 2524-2525` |
| Recursive comparison, hashing and text serialization of Arrays and Dictionaries stop at a depth of `MAX_RECURSION = 100` | `core/typedefs.h:260` |
| Beyond that depth, Array equality prints an error and returns true, and hashing returns 0 | `core/variant/array.cpp:148-151, 194-197` |

## Appendix B: Alternatives considered

| Alternative | Why it was not adopted |
|---|---|
| Borrowing by default, explicit move, or moving values into containers automatically | Too much annotation for scripting; shared references are used instead, and external types follow the engine's contracts |
| Tracing GC | Replaced by reference counting, weak references and manual cycle breaking |
| Plain references keeping Nodes alive automatically | Node lifetimes belong to the engine; handles do not extend them |
| HM-style inference with implicit generalization of ordinary functions | Conflicts with the engine calling functions by name (R2); semantics are defined first (R1–R7) and generics are explicit |
| Inferring a function's type from its callers | The type would change whenever a call is added, and dynamic callers are invisible (R1) |
| Rejecting a local whose constraints disagree (as Rust does) | In real code these disagreements are mostly "f64 in, f32 out", and rejecting them catches no real bugs; replaced by inflows first, then outflows (R4; see the survey) |
| Typing a local from its initializer alone (as Swift, Kotlin and C# do) | `var r = 0` followed by assigning a double would truncate the result to 0 (R4; see the survey) |
| An abstract int whose width is decided further along the call chain | Replaced by a full family of fixed-width types, with i32 when nothing constrains the type |
| f32 as the default float | f64 is the default; f32 uses the `f` suffix |
| A single warning class for all lossy implicit conversions | Precision loss, integer conversion loss and float-to-integer range loss are configured separately; integer conversions default to silent truncation (§7.3) |
| Boxing every outer enum variant solely because a single-field payload encoding includes NIL | The unique outer payload-free variant reserves NIL and the inner payload is boxed; full outer boxing is needed only if a conflict remains (§12.4) |
| Native signed INT for u64 stored in any, or boxing only large u64 values | any always boxes u64 to retain its unsigned value and a representation independent of magnitude; known u64 API slots use a separate bit-transport contract (§12.1, §12.3) |
| Bare NIL as a value of any | any is non-null; Option[any] uses NIL for absence, while an Option value stored inside any uses a typed non-NIL box |
| Rejecting all potentially lossy runtime integer conversions | Runtime conversions have defined modular results; only out-of-range constant conversions are mandatory errors (§7.3) |
| Overflow as undefined behavior when detection is off | It would break determinism (lockstep multiplayer, replays); the result is defined to wrap instead |
| Constants computed at runtime and then frozen | Only constant expressions are allowed |
| `T?` and Option as separate concepts | Generic code needs nesting, and two separate APIs for absent values would split the ecosystem; `T?` is `Option[T]` |
| Boxing enums as RefCounted objects | Objects compare by identity, which breaks Dictionary keys; read-only Arrays are used instead |
| Bit-packing small enums into INT or Vector4i | There is nowhere to store which variant a value is, float equality would depend on the representation, and the debugger would show raw numbers |
| Language-owned containers, copied at the boundary | Changes made on the engine side would be invisible; engine containers are used directly, with checks on read |
| Recursive enums with automatic indirection | Saves one keyword at the cost of invisible heap allocations; recursive enums are left out of the first version |
| Building a file-level dependency graph before inference | Dependencies only become known during inference; SCCs are formed per declaration and discovered on demand |
| Full row polymorphism and statically forwarding arbitrary arguments | Parameter packs are listed as future work |
| Hot reload and live state migration | Not planned |
