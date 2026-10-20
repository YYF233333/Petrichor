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

1. Strong static typing, inferred by default. If a type cannot be inferred, it is an error; there is no silent fallback to Variant.
2. Variant is an explicit boxed type. You can store and pass a Variant, but before doing anything specific with it you must check, narrow or convert it.
3. Method calls and operators produce capability constraints. Traits give these constraints names so they can be reused (§4).
4. There are named enums with payloads and exhaustive pattern matching. Option and Result are built from the same mechanism (§6).
5. Conditions must be bool. There is no general truthiness.
6. Single inheritance with overriding; no multiple inheritance. An override must honor the parent method's contract and must not narrow the parameters it accepts. Engine callbacks get their signatures from the engine contract.
7. No user-defined overloading. Default parameters, generics and overriding are kept. Engine methods that have several call forms are adapted by the binding layer.
8. The surface syntax uses Python/GDScript-style indentation and colons.

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
- **Flowing into a Variant position.** `print(x)` does not make x a Variant; otherwise it would be a silent fallback to Variant.

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

These rules are based on a survey of real Godot projects; see the [survey report](surveys/literal-local-constraints.en.md).

**Members are resolved in source order.** When you call a method or access a member on a value whose type is not yet determined, the constraints appearing before that point must determine the type under the rules above. Otherwise, it is an error asking for an annotation. If the value is a flexible literal with no constraints before that point, it takes its default type right there, and any later constraints become ordinary implicit conversions. This is the only case in R4 where the order of the code matters.

```
var a = 1
a.abs()             # no constraints before this point → a: i32
a = big_i64         # i64→i32: range-loss warning

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

- Inside the language, an enum is a plain value type and is never boxed. Only when it is converted to a Variant does §12.4 decide whether it is stored inline or boxed.
- **Enum values cannot be modified in place;** you construct a new value instead. Reference-type payloads are not deep-copied when an enum is copied.
- **[Known limitation] The first version does not support recursive enums.** An enum's value layout must not contain a cycle, including cycles through value types such as Option (e.g. `enum Tree: Node(i32, Tree?, Tree?)`). A cycle is a compile error. The diagnostic shows the cycle and suggests either using a class or recursing through Array or Dictionary. Recursion through those reference types is allowed, e.g. `enum Json: …, Arr(Array[Json]), Obj(Dictionary[String, Json])`. Godot has no lightweight box type, so every workaround goes through an Array or a RefCounted object. Demand is small, so the first version does not optimize for this.

### 6.2 `T?` is `Option[T]`

- `T?` is shorthand for `Option[T]`, and `null` is `None`. They are the same concept. Nesting is allowed: `T??` is valid and different from `T?`.
- Rationale: generic code needs nesting to tell "no value" apart from "the value is null". For example, `first[T](a: Array[T]) -> T?` with T = `Enemy?` must distinguish an empty array from an array whose first element is null.
- Option is an ordinary enum. The specification defines how it maps into Variant (the general rule in §12.4 yields `None` ↔ null), not how it is laid out in memory.

### 6.3 Narrowing

- After a null check, the type is narrowed only for **local variables**. Closures capture by value (§5.4), so no other code can change a local between the check and its use. Narrowing locals is therefore sound.
- Members, properties and array elements are not narrowed. Any call between the check and the use could change them: a method call, a synchronous signal, a setter, or an await.
- There is a syntax that checks a value and binds it to a local in one step, e.g. `if var t = target:` (spelling [Open]).

### 6.4 Nullability sugar

- **Postfix `!`** asserts that a value is non-null. If it is null, that is a program error, and the diagnostic gives the location. It guarantees only non-null, not that the object is still alive.
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

### 7.3 Implicit conversions and warnings

- Lossless conversions happen automatically. Whether a conversion is lossless depends on the source type's full value range: i32→i64, u32→i64, i32→f64 and f32→f64 are all lossless.
- Lossy implicit conversions are allowed but produce one of two kinds of warning:

| Warning class | Covers | Default |
|---|---|---|
| Precision loss | f64→f32; integer→float when precision may be lost (e.g. i32→f32, i64→f64) | Off |
| Range loss | Integer narrowing, sign changes (e.g. i32→u32), float→integer | On |

- Each class can be set independently to off, warn or error.
- If a constant can be represented exactly in the target type, neither warning is issued.
- An explicit conversion accepts the corresponding loss, but never causes undefined behavior (conversion semantics are in §7.5).
- Background: the engine itself mixes f32 and f64. In `_process(delta)`, delta is a double, while Vector2 components are real_t, which is float in standard builds. A line like `position += velocity * delta` must produce no warnings at all.

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
- Converting between u64 and Variant int reinterprets the bits (§12.3). A real-world test case: the ObjectID of a RefCounted object has bit 63 set, so as a u64 it is at least 2⁶³, and it becomes negative when passed through a Variant.
- Runtime bounds checking is separate from overflow detection and is on by default. [Known limitation] The first version offers no way to turn bounds checking off and has no unsafe mode.

## 8. Variables, constants and initialization

### 8.1 var and const

- The keywords are `var` and `const`; there is no separate `let` or `mut`.
- `const` accepts only constant expressions. There are no constants that are computed at runtime and then frozen, no `@onready const`, and no freezing phase.
- Members, construction, scene injection and `@onready` all follow Godot's lifecycle. The language adds no user-facing types for tracking initialization state.
- A non-null member must be initialized from a valid source; null can never be slipped into a T. Use `T?` for a value that may genuinely be absent.
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
- Engine Callables whose signatures are unknown keep a type-erased form. In the first version they are called through `Array[Variant]`/callv; the result is a Variant that must be narrowed.
- [Open] The following cases must be tested against the target version: the node's owner being destroyed, the node leaving the tree, the signal source being destroyed, the suspended frame being freed, and async values that cross language boundaries. Double resumption, leaks and invalid access must all be avoided.
- [Later] Variadic generics that keep full signatures (parameter packs). These are not a reason to adopt full row polymorphism.

## 10. Errors and diagnostics

- Failures that are expected and can be handled are returned as Result values and passed upward with a propagation syntax. Postfix `?` is reserved as the propagation token; its exact semantics are [Open].
- **Program errors** are: out-of-bounds access, invalid access, reading an uninitialized value, division by zero, a failed non-null assertion, integer overflow with detection on, and a failed entry conversion (§12.6). A program error produces a runtime diagnostic and a call stack. Where script execution stops is coordinated with the engine debugger; the whole game does not have to exit.
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

### 12.1 Principle

GDScript, the editor, Dictionary key comparison and resource files can all see how a value is represented in a Variant, so **the representation is part of the semantics**. The specification fixes each type's representation statically. It does not depend on the particular value, and it is fixed from the first version on; changing it later is a breaking change.

### 12.2 Containers

- **The language's containers are the engine's containers.** `Array[T]` and `Dictionary[K, V]` are the engine's Array and Dictionary, shared with the engine rather than copied. When the engine can express the element type, the engine's typed container is used.
- The type guarantee has two parts. Writes from the language are checked at compile time, and **every read is checked at runtime**. Reading a value that does not match T is a program error, with a diagnostic such as "element 3 is null, but the element type is Enemy (possibly written by external code)". An implementation may skip the read check when it can prove that only the language ever wrote to the container. That is an optimization and does not change the semantics.
- Rationale: the engine side can write anything into a container. The engine's typed Array allows null for Object elements, and an `Array[i32]` can only be an `Array[int]` on the engine side. If the language used its own containers and copied them at the boundary, it would not see changes the engine makes, which breaks the guarantee that an Array is a shared reference.
- Element types without a default value, such as non-null Object types, offer `resize(n, fill)` but not `resize(n)`. The engine's `Array::resize` leaves new Object elements as null.

### 12.3 Type mapping table

| Language type | Representation in Variant | Check when read back |
|---|---|---|
| i8…i64, u8…u32 | int | Value is in range |
| u64 | int, bits reinterpreted | None (values ≥2⁶³ appear negative, as with the engine's ObjectID) |
| f32, f64 | float | f32 is rounded; this is a precision loss |
| Core value types (Vector2, String, etc.) | The type of the same name | Type matches |
| Non-null Object class T | Object | Non-null and the class matches |
| enum (including Option) | See §12.4 | Fully decoded; see §12.5 |
| Function types | Callable | Signature is verified for Callables the language created; others are checked on every call |
| `Array[T]`, `Dictionary[K, V]` | Array, Dictionary | Each element is checked when read |

### 12.4 Enum mapping

**Representation set:** the set of Variant types that a type's values can be stored as. Integers and core value types have a single element. A non-null Object class is {OBJECT}. A nullable type includes NIL. Variant is every type. An enum's set is the union of its variants' sets, and a fully boxed enum is {ARRAY}.

**Rules:**

1. **Variants without payload:** if there is exactly one, it is represented as null; if there are several, each is represented by its index as an INT. A payload-free enum is therefore just an int, like a GDScript enum.
2. **Single-field variants:** represented exactly as the field is represented.
3. **Multi-field variants:** boxed as a read-only Array `[&"Enum.Variant", field1, field2, …]`. The leading StringName records the enum name and the variant name, without type arguments: both `Option[i32]` and `Option[String]` use `&"Option.Some"`, and the static type at the point of reading supplies the type arguments. Each field is stored in its own representation, and enum-typed fields apply these rules recursively.
4. **Conflicts:** the representation sets of the variants must not overlap. Several payload-free variants sharing INT does not count as a conflict. If there is any conflict, **all variants of that enum are boxed**, including payload-free variants (`[&"Enum.Variant"]`) and single-field variants (`[&"Enum.Variant", field]`).
5. Object payloads are told apart only by their Variant type (OBJECT), not by class, because subclasses overlap.

```
enum Cmd: Stop, Move(Vector3), Wait(f64), Hit(Node, i32)
# Stop→null  Move→Vector3  Wait→float  Hit→[&"Cmd.Hit", node, 5]   inlined
enum Hp: Dead, Alive(i32)              # Dead→null  Alive→int     inlined
enum Shape: Circle(f64), Square(f64)   # both float: conflict → all boxed
enum Key: A, B, Code(i32)              # index and Code are both int: conflict → all boxed
```

**Option is a special case of these rules.** `None` is the only payload-free variant, so its representation set is {NIL}; `Some(x)` has T's representation set. If T's set does not contain NIL, Option is inlined. If it does (T is itself nullable, an Option or a Variant, or an enum with exactly one payload-free variant), the sets conflict. In that case **only the outer Option** is fully boxed; the inner value keeps its own representation.

| Type | Representation set | `None` | `Some(…)` |
|---|---|---|---|
| `i32?` | {NIL, INT} | null | `Some(5)` → `5` |
| `Enemy?` | {NIL, OBJECT} | null | the object itself |
| `i32??` | {ARRAY} | `[&"Option.None"]` | `Some(None)` → `[&"Option.Some", null]`; `Some(Some(5))` → `[&"Option.Some", 5]` |
| `Cmd?` (Cmd from the example above) | {ARRAY} | `[&"Option.None"]` | `Some(Cmd.Stop)` → `[&"Option.Some", null]`; `Some(Cmd.Move(v))` → `[&"Option.Some", v]` |
| `Variant?` | {ARRAY} | `[&"Option.None"]` | `Some(null)` → `[&"Option.Some", null]` |

**Why this design:**

- **Boxes are read-only Arrays, not RefCounted objects.** Objects compare by identity, both with `==` and as Dictionary keys, so boxing the same value twice would produce two unequal objects. Arrays compare and hash by content and can be serialized directly. Because enums are immutable, sharing one box in several places behaves exactly like giving each place its own copy. Creating an Array also does not register anything in ObjectDB under a global lock, so it is cheaper than creating a RefCounted object.
- **No bit packing.** There is nowhere to store which variant a value is: a Variant has a single type tag, and copying a Variant copies only the value of its concrete type, so any spare bytes in the data area are lost. Bit-packed values would also show up in the debugger as meaningless numbers.

**Costs:**

- **Inlined values lose their type identity.** Once `Cmd.Move(v)` is stored in a Variant, it is just a Vector3. Narrowing from Variant to an enum can only check the shape, not where the value came from. In a Dictionary without a key type, `Cmd.Move(v)` and `v` are the same key. Boxed values keep their identity through the leading StringName.
- **Save compatibility.** Payload-free enums are saved by index, so reordering their variants breaks old saves, as in GDScript. Boxed values are saved by name, so renaming a variant breaks old saves.
- **Nesting depth.** If encoding would nest boxes deeper than the engine's recursion limit (`MAX_RECURSION`, currently 100), that is a program error. Beyond that depth, the engine's Array equality prints an error and **returns true**, hashing returns 0, and text serialization fails, so such a Variant must never be produced. Decoding enforces the same depth limit.

### 12.5 Reading back and decoding

- Inlined values: the Variant's type must belong to the representation set of one of the variants, and its content is then checked against that variant's field type (range, non-null, class).
- Boxed values: the value must be an Array whose leading StringName names an existing variant of the enum. Its length must be 1 plus that variant's number of fields. Each field is checked against its own type, and enum fields are decoded recursively.
- Reading a value back decodes it: the fields are copied into a value inside the language, and no reference to the external Array is kept. If external code later changes the variant name or the fields in that Array, the decoded value is not affected. Reference-type payloads (objects, Arrays, etc.) are still shared and are not deep-copied.
- When the language writes an enum into a Variant, it always creates read-only boxes. An implementation may reuse one box in several places, since nothing can tell the difference.

### 12.6 Entry conversion

Instances of the language's classes are Godot Objects, so GDScript and engine code can bypass the language's static types. Examples: `node.call("take_damage", "hello")`, `node.set("hp", 1 << 40)`, property values stored in a `.tscn` file, AnimationPlayer or Tween writing properties by path, or clearing `@export var target: Node2D` in the Inspector.

Every Variant that arrives from outside is converted and checked according to §12.3. This covers callp arguments, `set`, signal callback arguments, scene deserialization and Callable calls. A wrong type, an out-of-range value or a null in a non-null position is always a runtime error; values are never truncated silently.

### 12.7 Open questions

- Which types have a default value, for example whether an enum's default is one of its variants. This decides whether `resize(n)` is available.
- Whether narrow integer `@export` types such as i32 should get an automatic range hint, since the Inspector edits a 64-bit int.
- How the debugger should decode and display inlined enums and boxed values according to their static type.

## 13. Modules and engine facilities

The language keeps Godot's existing conventions for script files, resource paths and class registration. There is no new package manager and no elaborate visibility system. Exported properties, signals, resources, scenes and the editor reuse existing mechanisms wherever possible, but reuse alone does not prove compatibility; each still has to be verified. The first version does not specifically extend `@tool` or other ways of running scripts in the editor. These are supported if they work naturally, and otherwise deferred.

## 14. First-version scope

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
| Inference | Parameter types inferred from outflows; annotation required when only method calls are present; flowing into Variant is not evidence; for locals, inflows first and outflows second, with results independent of the order of constraints; flexible literals fixed to their default type at a method call; empty-container members require annotation; branches returning different classes are not merged into a common ancestor |
| Generics | Explicit generics instantiate; generic methods are invisible to `has_method`; instantiating with i32 gives 32-bit semantics |
| SCC | Mutual recursion resolves within a file and across files (including the `make_b().g()` example in §11); generic utility functions outside the cycle are unaffected; conflicts are reported at their real source |
| Enums | Payloads, named returns and exhaustiveness checks; no anonymous sum types are synthesized; enum values cannot be modified in place; a recursive enum is an error that shows the cycle, while recursion through Array, Dictionary or a class compiles |
| Traits | Compile results when a constraint is satisfied and when it is not, without relying on dyn |
| Nullability | `T?` and `Option[T]` are equivalent; `T??` distinguishes its two levels; the diagnostic when `!` fails; `?.` short-circuits the whole chain; members are not narrowed |
| Liveness | A local Node held across an await is still non-null but may no longer be alive; `?.` on a freed object reports invalid access |
| Ownership | A RefCounted object held only by an Object-typed variable stays alive; ownership is unchanged after downcasting; `weak` on a handle type is an error |
| Sharing and write-back | Changes made through one alias are visible through the others; `line.points.append(p)` writes back to the property; explicit copies never secretly deep-copy; weak back-references and manual cycle breaking work |
| Numbers | `position += velocity * delta` produces no warnings; each warning class can be set to off, warn or error; with `x: u8`, `x + 100` has type u8; `var a: u8 = 300` is a compile error |
| Overflow | Overflow in a constant expression is a compile error; explicit wrapping operations never report errors; turning runtime detection on and off works; the interpreter and JIT agree with detection off; an overflow that can only be shown across functions is not a compile error |
| Numeric edge cases | Every row of the table in §7.5; u64 round trip (bit 63 of ObjectID); bounds checking is unaffected by the overflow detection setting |
| Initialization | A construction-time member initializer that references self is an error, while `@onready` initializers may reference self; an `@onready` initializer that reaches a later-declared member through a method call is caught; `_init` is checked to be callable with no arguments; a child's `_ready` reading a parent's `@onready` member through a signal produces a readable diagnostic |
| await | Resumption order, reentrancy, resuming after the instance is destroyed, and frame cleanup, all compared against the target version |
| Callable | Known signatures are checked statically; type-erased results never pose as a known type |
| Variant mapping | Every row of the mapping table round-trips; each enum rule, including boxing every variant on conflict; nested Options round-trip as in the table in §12.4; boxed values used as Dictionary keys match by content; exceeding the depth limit is an error; decoding checks the variant, the length and every field; changing a box externally after decoding does not affect the decoded value |
| Entry conversion | GDScript passing a wrong type, an out-of-range int or null each produces a readable error and leaves the instance intact; nulls produced by `Array::resize` are caught on read |
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
| A single warning class for all lossy implicit conversions | Losing precision and losing range carry very different risks, so there are two classes, configured separately |
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
