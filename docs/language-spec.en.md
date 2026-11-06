# Petrichor Language Specification

**v0.2 draft · 2026-10-25** · 中文: [language-spec.zh.md](language-spec.zh.md)

Petrichor is a statically typed scripting language for medium-to-large Godot 4 projects, delivered as a GDExtension. It replaces GDScript in project code and coexists with GDScript plugins. This document is its draft specification; the [implementation](architecture.md) does not cover every rule yet. Design trade-offs and corpus evidence are in [design-notes.md](design-notes.md), and the engine verification baseline is in [engine-facts.md](engine-facts.md). Version v0.1 is kept at git tag `spec-v0.1`.

## 1. Overview

### 1.1 Problem and goals

- GDScript cannot leave dynamic typing behind because of compatibility. Static information in scenes never reaches scripts: node paths, signal connections, animation tracks and resource properties. C# has second-class language support, and its bridge and toolchain have pitfalls of their own.
- This language is statically typed throughout; dynamic values appear only where the engine or third parties impose them. Static checking extends to scenes. Node paths, scene signal connections, animation tracks, exported properties and scene resources are typed. Renames update scenes too.
- It targets medium-to-large projects and developers who read and modify code, part of it possibly AI-generated. Small projects are better served by GDScript. New projects are the main use, while third-party plugins stay in GDScript.

### 1.2 Relationship to GDScript

- Surface syntax follows GDScript: indentation, `func`, `var`, `extends`, `signal`, `@export`, `$X` and so on. New constructs extend it in the same style.
- **Syntax hard rule**: a construct spelled as in GDScript either behaves as in GDScript or is a compile error. The error comes with a suggested fix, and the construct is never silently different. The sole exception is where GDScript fails at runtime or silently yields a wrong result. Examples are a default value, a truncation and waiting forever. In such cases this language raises a program error, or does what the code intended.
- Compiling GDScript source directly is not a goal; migration is guided by compile errors and quick fixes.

### 1.3 Delivery

- Delivered as a GDExtension that installs in the official editor and uses the official export templates. Web exports require "Extensions Support" enabled (cross-origin isolation headers are needed only with thread support).
- Source is compiled only in the editor and headless mode. Exported games contain only bytecode, and export-template libraries omit the compiler.

### 1.4 Conventions

- Rules defining legal programs and required behavior are normative; examples, [design-notes.md](design-notes.md) and [engine-facts.md](engine-facts.md) are not.
- Status markers: **[Open]** means not yet decided. **[Known limitation]** means unsupported in the first version, though its legality restrictions remain normative. Open questions and compatible additions after the first version are collected in the chapter on first-version scope.
- A **compile error** is reported by the analysis service before running and makes the program illegal. The exception is a missing-value error marked on a scene, which does not block running in the editor. **Warnings** and **hints** do not affect legality. A **program error** is a runtime error; the chapter on errors defines its handling.
- Engine behavior relied on here was checked against Godot 4.7.2-stable (commit `ed1daf0`), with sources in [engine-facts.md](engine-facts.md).
- Petrichor and `.pet` are provisional and will be finalized before the first release. The extension must be short and lowercase. It must not be used by Godot, GitHub Linguist or common editors.

## 2. Lexical structure and surface syntax

### 2.1 Files and classes

- `preload` of any script yields its class. A constant initialized with it names that class where its bare name is used. That name works like a `class_name`: as a type, and with `new()`, static methods and constants. `preload(...)` followed directly by `.new()` or another member works the same way. Where a value is expected, it is the script resource itself. Its type is `GDScript` for `.gd` files, and its script resource class otherwise:

  ```gdscript
  const Unit = preload("res://units/unit.gd")

  var squad: Array[Unit] = []

  func spawn(at: Vector2) -> Unit:
      var unit := Unit.new()
      unit.position = at * Unit.TILE_SIZE
      return unit

  func adopt(body: Node2D):
      if body is Unit:
          squad.append(body)
      else:
          body.set_script(Unit)
  ```

- A class header may contain, in order: class annotations, `class_name`, `extends`, `implements`. The last two may share `class_name`'s line or take lines of their own:

  ```gdscript
  class_name Player extends CharacterBody3D implements Combat.Damageable
  ```

- A file may declare inner classes, structs, enums and interfaces, all belonging to the file's class. Inner classes are written as in GDScript, and `implements` may go wherever `extends` may.

### 2.2 Lexical structure

- Indentation, comments, identifiers, literals and operators follow GDScript, with these changes:
  - New keywords: `pub`, `struct`, `interface`, `implements`, `async`, `None`.
  - New postfix operator `?` for short-circuit return.
  - `null` is an alias of `None` everywhere, becoming NIL in a Variant. Assigning it to a non-null `T` is a compile error. Its quick fix changes the type to `T?`. A hint suggests `None` in new code.
  - Single- and double-quoted strings cannot span lines; multi-line strings use triple quotes.
  - `##` documentation comments also document structs and their fields, and interfaces and their members. A comment beginning with `###` is an ordinary comment.

### 2.3 Annotations

- [Known limitation] Scripts do not run in the editor in the first version, so `@tool` and `@export_tool_button` are compile errors.
- `@warning_ignore` takes the snake_case names of this language's warnings and hints. Each hint is named as a warning is. A name that suppresses no diagnostic, including GDScript warning names left from migration, gets the hint `unused_ignore`.
- New class annotations: `@parent(T)` and `@owner(T)`, for upward access; new member annotation: `@readonly`, for interface properties only.
- `pub` goes after annotations and before `static`.

### 2.4 Declaration spellings

- **Structs**: `struct Name:` followed by `var` fields with optional defaults; construction takes named arguments only, omitting fields with defaults:

  ```gdscript
  struct Hit:
      var damage: int
      var point: Vector3
      var crit: bool = false

  var h = Hit(damage = 3, point = p)
  ```

- **Enums**: variants may carry named payload fields and are constructed by named arguments. Payloadless enums match GDScript:

  ```gdscript
  enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

  var c = Command.MOVE(target = Vector3.ZERO)
  ```

- **Interfaces**: an indented block declaring `var`, `signal` and `func`. Bodyless methods omit the colon; methods with bodies are default methods. Properties require read-write unless marked `@readonly`; all interface members are public:

  ```gdscript
  pub interface Damageable:
      var hp: int
      signal died
      func take_damage(amount: int)
      func is_dead() -> bool:
          return hp <= 0
  ```

- **Implementing and extending interfaces**: classes write `implements A, B`; interfaces write `interface Combatant extends Damageable, Healable:`.
- **pub**: keyword used on members, structs, enums, interfaces and inner classes, as in `@export pub var max_hp: int` and `pub static func make() -> Player`.
- **Placement and naming**: structs, enums, interfaces and inner classes belong to their file's class. They are named bare in that file and as `ClassName.Name` elsewhere (e.g. `Combat.Hit`).
- **Function types**: shaped like function declarations without function or parameter names:

  ```gdscript
  var on_hit: func(int, Node3D)                 # void return; -> void may also be written
  var score: func(Enemy) -> float
  var load_level: async func(String) -> Level   # calling yields Task[Level]
  var filter: (func(Enemy) -> bool)?            # parentheses required for nullable function type
  ```

### 2.5 Statements

- Statements are spelled as in GDScript.

## 3. Types

### 3.1 Overview

| Category | Types this language adds or changes |
|---|---|
| Objects | non-null `T` and nullable `T?`; `PackedScene[T]` |
| Value types of this language | structs, enums |
| Interfaces | interfaces |
| Standard types | `T?` (Option), `Result[T, E]`, `Task[T]` and `Settled[T]`, `weak[T]`, `ConversionError` |
| Functions | function types |

Numbers, strings, core value types, containers, `Callable`, `Signal` and `Variant` are GDScript's types. The sections below state only what differs.

### 3.2 Numbers

- There are no narrow integer types. Fixed-width arithmetic uses wrapping methods and bit operations on `int`.
- `int` converts implicitly to `float` in assignments, arguments and returns. Converting `float` to `int` requires `int(x)`. Without it, the conversion is a compile error.

### 3.3 Objects and nullability

- Object type `T` is non-null; `T?` is an object or `None`.
- After an object is freed:
  - every read of a `T?` reference checks liveness, as `is_instance_valid` does, and yields `None` for a freed object. So `!= None` narrowing is also a liveness check;
  - a `T` reference may still hold a freed object. Reading it, comparing ObjectIDs with `==` and ordinary argument passing do not count as access. Accessing its members is a program error. For an instance of this language or an object this language freed, a debug build names where it was freed;
  - passing a freed object to an object-typed engine parameter is a program error. So is writing it to an object-typed engine property. So is passing it to a method of an object-typed `Array[T]`, such as `append`, `erase` or `has`. `Variant` parameters are unaffected (`is_instance_valid` returns `false`);
  - for every static type, `is` on a freed object is `false` and `as` yields `None`;
  - if a running method's object is freed or loses its script of this language, the method may return normally. Accessing any member or `self` afterwards is a program error.

### 3.4 Option

- `T?` is Option: `Some(v)` or `None`. A `T` value wraps into `Some` implicitly, or explicitly as `Some(v)` (such as `Some(None)` in `T??`). When `S` is a subtype of `T` or implements interface `T`, `S?` is usable where `T?` is expected.
- Operations are methods named as in Rust. `unwrap` and `expect(message)` raise a program error on failure. The others are `unwrap_or`, `unwrap_or_else`, `map`, `and_then`, `or`, `or_else`, `filter`, `ok_or` and `ok_or_else`. There are no `!`, `??` or `?.` operators; the only special syntax is short-circuit postfix `?`. Null checks use `!= None` and narrowing.
- Nested Options keep distinct levels (such as `T??`):
  - implicit wrapping adds levels to match the target type: a `T?` assigned to `T??` becomes `Some` holding it. Literal `None` is always the outermost `None`;
  - `?`, `unwrap`, `match` and narrowing each peel one level, and `.flatten()` merges `T??` into `T?`;
  - `Variant?` works the same way: `None` is absent, and NIL is a value.

  ```gdscript
  var r = enemies.pop_back()   # enemies: Array[Enemy?], r: Enemy??
  match r:
      None: print("empty array")
      var e: print(e)          # e: Enemy?, possibly None
  ```

### 3.5 Result

- `Result[T, E]` is constructed with `Ok(v)` and `Err(e)`; success values never wrap implicitly.
- Methods match Rust: `unwrap`, `expect`, `unwrap_or`, `unwrap_or_else`, `map`, `map_err`, `and_then`, `or_else`, `ok`, `err`; postfix `?` returns `Err(e)` on `Err(e)`.
- Discarding a `Result` is a compile error; write `.ok()` to ignore it deliberately.

### 3.6 Structs

- Structs have value semantics: assignment, argument passing and return make shallow copies, sharing object and container references. Structs exist only inside this language. They take named constructor arguments.
- Across the engine boundary, a struct encodes automatically as a `Dictionary` with `String` field-name keys. Decoding checks every field and nested container or struct by the rules of `T.try_from`. It ignores unknown keys.
- [Known limitation] Structs cannot be `@export`ed or saved in `.tres` in the first version (use `Resource` for editor data).
- Structs may declare `func` and `static func`. `self` is read-only in instance methods, and assigning to a field of `self` is a compile error.
- Fields and methods are visible wherever the struct is.
- `s.x = 1` mutates the storage holding `s`:
  - chained assignment reads, modifies and writes back along the chain (`player.last_hit.damage = 3` runs `last_hit`'s setter, and `hits[0].damage = 3` updates the array element);
  - assigning to a field of a temporary (such as `get_hit().damage = 3`) is a compile error;
  - loop variables and lambda captures are copies. Mutating a field of a struct loop variable without reading the variable afterwards triggers the `modified_copy` warning. Mutating shared state through an object or container field does not.
- A struct provides field-wise `==` and hashing over all fields, so any struct may be a `Dictionary` key. Hashing takes containers by content. When every field has `<`, it also provides `<`, lexicographic in declaration order. Object fields have no `<`, so neither does a struct with one.
- A struct or payload enum containing itself, directly or indirectly, without going through `Array` or `Dictionary` is a compile error. The error lists the cycle's fields. Recursion through a container is valid.

  ```gdscript
  struct Hit:
      var damage: int
      var crit: bool = false

      func total() -> int:
          return damage * 2 if crit else damage

  player.last_hit.damage = 3   # read, modify, write back through last_hit's setter
  get_hit().damage = 3         # compile error: modifies a temporary
  for h in hits:
      h.damage = 0             # warning: modified_copy
  ```

### 3.7 Enums

- An explicit enum value must be an `int` the compiler computes. An anonymous `enum { A, B }` declares `int` constants of the class.
- Enums of this language are closed and checked at entry. Engine enums are open: the engine produces values outside them and adds values in newer versions. So `MouseButton(x)` performs no range check, and a `match` over one requires `_`.
- `int` converts to an enum of this language only explicitly. Write `State(x)`, a program error if out of domain, or `State.try_from(x)`:

  ```gdscript
  enum SpeedUnit { KMH, MPH }
  var unit := SpeedUnit.KMH
  unit = SpeedUnit((unit + 1) % SpeedUnit.size())
  unit = 1                                          # compile error; quick fix writes SpeedUnit(1)
  ```

- Used as a value, an enum's name has typed methods: `keys() -> Array[String]`, `values() -> Array[State]`, `find_key(v) -> String?` and `get(name) -> State?`. `State[name]` has type `State` and is a program error when the name is missing. A constant initialized with an enum name aliases that enum.
- An enum without the value 0 has no default value, so its variables must be assigned.
- Enums with payloads:
  - payload fields must be named (e.g. `MOVE(target: Vector3)`) and can be extracted only via `match`; `c.target` is a compile error;
  - across the engine boundary they encode as field-name `Dictionary` values. The reserved key `"$variant"` holds the variant name, as in `{"$variant": "MOVE", "target": p}`, or `{"$variant": "IDLE"}` without payload;
  - `==`, `<` and hashing follow the rules of structs, ordering by variant declaration order first and then by fields;
  - [Known limitation] in the first version they cannot declare methods or be `@export`ed.

### 3.8 Containers

- Typed containers are invariant in their type arguments (`Array[Node2D]` is not assignable to `Array[Node]`).
- **Typed and untyped conversions**: an untyped container converts to a typed one only explicitly. When the runtime container is an `Array[int]`, `a is Array[int]` is true and narrows without copying. `assign()` copies elements. Through an untyped view, writing a value incompatible with the element or key type is a program error. So is looking one up through it, with `has`, `find`, `erase` and so on. So is `assign()` encountering one. `None` written into `Array[Node]` is a program error when read.
- **Representation and nesting**:
  - engine-representable element types use the engine's shared typed container;
  - language-only element types, such as structs and payload enums, use an internal representation. An untyped engine copy, encoded by the engine-boundary rules, requires explicit `Array(hits)` or `Dictionary(d)`. The reverse is `Array[Hit].try_from(v)`;
  - nested containers (such as `Array[Array[int]]`) preserve inner types, checked at entry and on read.
- **Element access and methods**:
  - `pop_front()` and `pop_back()` return `T?` (`T??` when `T` is nullable, outer `None` meaning empty);
  - `pop_at(i)`, `front()`, `back()`, `min()` and `max()` return `T`. On an empty array or out-of-range index they raise a program error, as subscripting does;
  - `Dictionary[K, V].get(k)` returns `V?` (`None` if missing), `get(k, default)` returns `V`, and `d[k]` on a missing key is a program error;
  - all methods use the elements' own `==` and `<`. `resize` fills element defaults, and empty typed inner containers for nested containers. `duplicate(true)` deep-copies containers inside elements;
  - ordering methods are compile errors when the element type lacks `<`. On `Array[Node]` the error offers `sort_custom`. [Known limitation] Ordering `Variant` elements treats incomparable pairs as not less, as in GDScript, with `min`/`max` returning `null`. `resize` is a compile error when the element type has no default. On `Array[Node]` it offers `Array[Node?]`;
  - the standard library adds `map`, `filter`, `sort` and similar typed-lambda helpers on native containers.
- **Dictionary keys**: inserting a new key stores a copy, isolating the key from later mutations of the original. The copy's containers are read-only at every level. They are shared within it as in the original, a container that holds itself included. `keys()`, `for k in d` and `find_key()` hand out that read-only copy. Mutating it is a program error, suggesting a mutated `duplicate()`. Lookups neither copy nor freeze their argument. Container keys of foreign `Dictionary` values are made read-only in place when obtained.

### 3.9 Interfaces

- Nominal interfaces: classes must declare `implements`, and only classes of this language can implement interfaces. Implementing members must be `pub`.
- Interfaces may declare methods (including default methods), properties and signals. Inherited engine properties can satisfy matching property requirements. Properties require read-write unless marked `@readonly`. So read-write members satisfy `@readonly`, while setter-less engine properties satisfy only `@readonly`. Assigning to a `@readonly` property in a default method, or using `@readonly` outside an interface, is a compile error.
- An interface may `extends` several parent interfaces. Child values convert to parent interfaces. A child interface may covariantly narrow a parent method's return type. Incompatible parent member signatures or extension cycles are compile errors. Conflicting default methods from multiple parents must be overridden.
- [Known limitation] The first version has no stateful mixins and no generic interfaces. `interface Source[T]:` is a compile error.

### 3.10 Generics

- The first version provides only built-in generic types (`Array[T]`, `Dictionary[K, V]`, `Result[T, E]`, `Task[T]`, `weak[T]`, `PackedScene[T]`); user code cannot declare type parameters.

### 3.11 Function types and Callable

- A function type records parameter types, return type (`void` when omitted) and `async`. Calls use `f.call(x)` and are checked statically; `f(x)` is a compile error offering `f.call(x)`.
- Compatibility is identical for method values and lambdas: contravariant parameters, covariant return. `async` and ordinary function types do not convert implicitly to each other. Engine-invoked positions accept `async` functions and run them as root tasks. These are signal connections in code or scenes, and engine `Callable` parameters such as `tween_callback`.
- Method values and lambdas passed to the engine become `Callable`s bound directly to the target method, not by name. `f.bind(a, b)` drops the last two parameters from `f`'s function type. An untyped `Callable` requires an explicit conversion to a function type and checks arguments and return value on every call.
- Lambdas capture by value, as in GDScript. They differ in two ways:
  - assigning to a captured variable, or to a value-type field of one, inside a lambda is a compile error. Keep shared mutable state in a member or container. Because captures cannot be reassigned, outer narrowing holds inside the lambda;

  ```gdscript
  var hits := 0
  area.body_entered.connect(func(body): hits += 1)        # compile error: assigns captured variable
  var counter: Array[int] = [0]
  area.body_entered.connect(func(body): counter[0] += 1)  # valid: shared container
  ```

  - lambda parameters cannot have default values; writing one is a compile error.

### 3.12 Weak references

- `weak[T]` is created with `weak(x)`, and `.get()` returns `T?`. To capture `self` weakly in a closure, bind `var me = weak(self)` outside and call `me.get()` inside.

### 3.13 Variant

- **Into Variant**: structs and payload enums are encoded by the engine-boundary rules. Containers of language-only types do not enter implicitly; they require `Array(hits)`. Array and dictionary literals entering `Variant` stay untyped, including elements of untyped containers. Without an expected type, they infer from their elements, as `var a = [1, 2]` is `Array[int]`.
- **Out of Variant**: conversion is always explicit. Objects use `v as T`, yielding `T?`, and `None` when not an object. `T(v)` is constructor conversion, and `T.try_from(v)` strict conversion. `if v is T:` narrows locals and parameters.
- Only `==` and `!=` apply directly. All other operations are compile errors offering conversions or `is`: arithmetic, `v.x`, `v.f()`, `v[i]`, `for x in v`. Dynamic access by name converts to `Object` first and uses `get`/`set`/`call`.
- `Variant` includes NIL. `v == None` is true for NIL and freed objects, `v is int` is false for NIL, and `v as Node` yields `None` for NIL. `Variant?` adds an `Option` layer (for `Dictionary[String, Variant].get("hp")`, a missing key is `None` while `{"hp": null}` is `Some(NIL)`).

  ```gdscript
  var data = JSON.parse_string(text)   # Variant
  data.hp                              # compile error: convert first
  if data is Dictionary:
      var hp = int.try_from(data.get("hp", 100)).unwrap_or(100)
  var n = data as Node                 # Node?
  ```

## 4. Declarations

### 4.1 Classes and inheritance

- Class names share the engine's flat global class table; duplicates across this language, GDScript plugins and C# are compile errors. [Known limitation] There are no namespaces in the first version.
- A class may extend an engine class or a class of this language. [Known limitation] Cross-language inheritance is unsupported in both directions.
- **Bare-name lookup**: a bare name resolves first to locals and declarations of the class and its bases in this language. It then resolves to members of its native base chain:
  - properties, methods, signals and constants;
  - enum values, typed as that engine enum;
  - enum names, usable as types; bitfield values and other integer constants are `int`.
- The native base supplies names of constants, enum values and enums. Declaring a variable, constant, signal, anonymous enum value or inner class with such a name is a compile error. In an inner class, a bare name on both an enclosing class and the native base is a compile error. Write `Outer.X` or `Area3D.X` instead.
- **`@abstract`**: abstract methods omit the colon and body. A class with abstract methods must be marked `@abstract`. Concrete subclasses must implement all abstract methods. Abstract classes cannot be constructed or attached to scene nodes (marked on the scene). Calling an abstract method via `super` or marking a `static` method `@abstract` is a compile error.

  ```gdscript
  @abstract
  class_name BattlerAction extends Resource

  @export var label: String
  @abstract pub async func execute()
  ```

- **Inner classes**:
  - may declare everything a file class can (including nested inner classes), except `class_name` and class annotations other than `@abstract`;
  - may extend same-file inner classes by bare name, external ones as `Other.Inner`, or an enclosing class. Without `extends`, they extend `RefCounted`;
  - resolve own and inherited names first, then nearest-first non-instance declarations of enclosing classes. These are constants, types, static functions and static variables. Referencing an enclosing instance member is a compile error stating there is no outer instance;
  - are accessed via `Outer.Inner` or an outer instance (such as an autoload);
  - are standalone pathless, unnamed scripts at runtime (`get_script()` returns them), attachable via `set_script()` but not in scenes.

### 4.2 Visibility

- Members without `pub` are visible within their file and in subclasses, and invisible elsewhere. This covers structs, enums, interfaces and inner classes. Their file includes its inner classes. Subclasses may read, write, call and override them. `pub` members are visible everywhere.
- Overrides must keep visibility; interface implementations must be `pub`; a non-`pub` type in a `pub` signature is a compile error.
- `new(...)` takes the visibility of the nearest `_init` up the inheritance chain. Without any `_init`, `new()` is unrestricted. If `_init` is not `pub`, only its file and subclasses may call `new(...)`.
- A non-`pub` must-assign member must be exported for scenes or assigned in its file or subclasses. Otherwise its declaration is a compile error. Which members reflection by name sees is in the section on members exposed through reflection.

### 4.3 Member variables

- A `var` with an initializer may infer its type like a local. For example, `var speed = 5.0` is `float`, and `@onready var label = $Label` infers from scenes. Hover, completion and reflection use the inferred type. Annotations are required without an initializer, or when the initializer is ambiguous, such as empty containers or `None`.
- Without an initializer:
  - numbers, `bool`, strings, core value types, containers, `Callable`, `Signal`, and payloadless enums containing 0 take their default. `T?` is `None`, `Variant` is NIL, and `weak[T]` is empty;
  - a struct takes its default when every field has one. Otherwise it is a must-assign member. So are non-null object references, payload enums, enums without 0 and function types.
- `@export` matches GDScript except in three points. Structs and payload enums cannot be exported. Exported scenes use `PackedScene[T]`. Non-null object exports must be assigned.

### 4.4 Property accessors

- Inline `set(value):` / `get:` and named `set = f` / `get = g` match GDScript. Inside its own accessors, the property name accesses the backing field. All other reads and writes in the class go through the accessors. Initializers bypass setters. Accessors cannot be coroutines.

### 4.5 Constants

- `const` may appear in classes or functions. Class constants are named bare in their class, subclasses and static functions. Elsewhere they are named as `Player.MAX`, and also via instances (`self.MAX`, `p.MAX`). Omitted types infer as for member variables: `const SPEEDS = [1.0, 2.0]` is `Array[float]`. `None` requires an explicit `T?`.
- Initializers must be constant expressions, which exclude method calls, lambdas, `self` and variables. They are built from:
  - literals, constants, enum values, and engine and built-in constants;
  - `preload`, operators and conditional expressions;
  - array and dictionary literals, and built-in constructors and conversions;
  - engine math utility functions;
  - subscripts and fields of constants.
- **Evaluation split**:
  - the compiler computes `int`, `float`, `bool`, the three string types and operations or conversions among them;
  - the compiler also computes math functions returning `int` or `bool`, and the structure of container literals;
  - constructions of and operations on engine value types (vectors, `Color` and so on) are evaluated once when the class loads;
  - so are math functions returning `float`, and `**` with a `float` operand.
- **Compile-time value checks** ("known" means computed by the compiler, guaranteeing load-time evaluation never fails):

  | Case | Compile-time requirement |
  |---|---|
  | `/` and `%` on `int` and integer vectors, `posmod` | Divisor (each vector component) known and non-zero |
  | `<<` and `>>` | Both operands known and non-negative, shift below 64 |
  | `**` on two `int`s | Both operands known; base non-zero when exponent is negative |
  | `float` to `int` (`int(x)`, `floori(x)` and so on) | `x` known, finite, and within `int64` |
  | Subscript or key of a constant | Index or key known, in bounds / present |
  | Keys of a dictionary literal | Known and distinct |
  | `Color(s)` | `s` a hex literal (color names are compile errors whose quick fix writes `Color.RED` and the like) |

- Annotation arguments, enum values and positions required "known" above accept only compiler-computed values. [Known limitation] An `int` rounded from a load-time `float`, such as `floori(sin(X) * 100)`, cannot be used there. Constant expressions also exclude:
  - `%` formatting, `is`, `as` and `in`;
  - GDScript-only utilities (`Color8`, `len`, `char`, `ord`);
  - `Basis`/`Quaternion` constructors and multiplications whose argument values debug builds assert on.
- Arrays and dictionaries in constants are read-only at every level. Direct mutation is a compile error. Mutation through another variable is a program error. Changing a constant counts as a structural change for hot reload.

### 4.6 Functions

- Function signatures are never inferred. Parameters must be annotated, except in overrides and in lambdas with an expected type. An omitted return type on a non-override means `void`, and returning a value is then a compile error. Parameters may have defaults.
- **Overrides**: overrides of engine virtuals or base methods may omit parameter and return types. They then inherit them, e.g. `delta: float` in `func _process(delta):`. Written types allow covariant returns, contravariant parameters, extra parameters with defaults, and renamed parameters.
- **Rest parameters**: a rest parameter must be the last, defaultless parameter `...args: Array` or `...args: Array[T]`. Its arguments are checked one by one statically, and at entry from outside. Bare `...args` is a compile error. There is no call-site spread; pass arrays with `callv`.
- Ordinary calls are positional. Named arguments apply only to struct and payload-enum constructors and to must-assign members in `instantiate(...)` and `new(...)`.
- Locals infer from initializers. Lambda parameters take their types from an expected function type when present, and require annotations otherwise. Lambda return types infer from the body.
- Within one function, locals, `for` variables and `match` `var x` bindings cannot shadow a local or parameter of an enclosing block. Sibling blocks may reuse names. A lambda body counts as an inner block of the enclosing function. Lambda parameters may still reuse outer local names.
- Coroutines (lambdas included) must write `async func`.

### 4.7 Signals

- Signal parameters must be annotated, e.g. `pub signal died(cause: String)`.
- `emit`, `connect` and scene signal connections check argument counts and types statically. Engine signals take types from ClassDB, with unaudited object parameters non-null.

### 4.8 Static members

- Static initialization never runs in the editor. When a class loads, base classes first, its `static var` initializers run in declaration order and bypass setters. Parameterless void `static func _static_init()` runs next, and then inner classes in turn.
- A `static var` is shared with subclasses and may have accessors. It cannot be `@export` or `@onready`. Without an initializer it defaults as member variables do, and types without defaults, such as non-null objects, are compile errors. Without `@static_unload`, a class with static variables stays loaded for the run.
- Static variables and static methods are named bare in their class, subclasses and inner classes. Elsewhere they are named via `Registry.count` / `Base.label()`. They are also accessible on instances or values:
  - `pub` static variables are readable and writable by name from GDScript on the class and its objects;
  - on script objects, static methods dispatch from the runtime class upward;
  - on engine classes and built-in values, static methods call the type's own method.

  ```gdscript
  var b: Base = Sub.new()
  b.label()                             # Sub.label()
  Base.label()                          # Base.label()
  var hue := Color.RED.from_hsv(0.5, 1.0, 1.0)
  ```

- Static initialization cannot `await`. Reading another class's static member initializes that class first. Directly reading a later-declared static variable of the same class is a compile error. Reading or writing an uninitialized static variable through a call is a program error. So is a cross-class static initialization cycle, whose error names the dependency chain.
- On hot reload, adding, removing or altering static member declarations requires a restart. Editing only function bodies, initializers or `_static_init` keeps existing static values, without re-running static initialization.

## 5. Expressions and statements

### 5.1 Operators

- A shift with a negative left operand is a program error. So is a shift count below 0 or at least 64. In constants, both are compile errors.
- In `x in c`, `x` must be assignable to the element or key type. For example, `1 in scores` is valid on `Array[float]`, while `1.5 in ids` on `Array[int]` is a compile error. `name in obj` on a freed object is a program error.
- `int ** int` is exact and wraps on overflow like `*`. Negative exponents truncate `1 / (a ** -b)` toward zero: `2 ** -1` is 0, and `(-1) ** -3` is -1. `0` to a negative power is a program error.
- In `%` formatting, language-only types enter as the engine boundary encodes them, and their containers need `Array(hits)`. A format mismatch, such as `"%d %d" % [1]`, is a program error.

### 5.2 Comparison

- `!=` is always the negation of `==`. `==` differs from GDScript as follows:
  - objects compare identity by ObjectID, so two distinct freed objects are unequal. Comparing two unrelated class types is a compile error; interfaces are allowed;
  - `Option`: `None == None` is true, and two `Some`s compare contents. The shallower side wraps to match levels, as for `int?` with `3` or `T?` with `T??`. Comparing a non-`Option`, non-`Variant` value with `None` is a compile error, offering `T?` or `is_instance_valid(x)`;
  - containers compare element-wise or entry-wise, requiring matching runtime element types. So `[1] == [1.0]` is false, while `NaN` matches `NaN`. Comparing statically distinct container types, such as `Array[int] == Array[float]`, is a compile error. Structs and payload enums match field by field;
  - for a `Variant`, `v == None` is true on NIL and on freed objects.
- Ordering:
  - `Array` orders lexicographically when its element type has `<`. At the first position where the elements do not match under container `==`, the result is their `<`. When all match, the shorter is less;
  - `T?` places `None` before `Some` when `T` has `<`, and **never** wraps implicitly: `hp < 3` on `hp: int?` is a compile error;
  - structs and payload enums order as their sections define;
  - for these three, `>`, `<=` and `>=` derive from `<` and `==`: `a > b` is `b < a`, and `a <= b` is `a < b or a == b`. Other types take all four from the operator table.

### 5.3 Conversions

- `x as T` applies only to object types and yields `T?`. It yields `None` on mismatch, freed objects or non-object `Variant`s. When `x` already has static type `T`, it yields `T` with a redundancy hint. Required downcasts write `(x as T).unwrap()`. For `is_in_group` followed by a cast, quick fixes suggest `is`.
- `as` to a built-in type or enum is a compile error. Its quick fix offers `T(x)`, the constructor or explicit cast, as in `int("5")`, `int(2.7)` and `State(x)`. It also offers strict `T.try_from(x)`.
- `T.try_from(v)` returns `Result[T, ConversionError]` using struct-field decoding rules. The runtime type must match `T`. `int` also accepts exact integer `float`s within `int64`. For an enum of this language, out-of-domain `int`s yield `Err`.

### 5.4 Narrowing

- Locals and parameters narrow after `!= None`, `is` and early returns. They also narrow in branches of `a if c else b`, and on the right of `and`/`or`. Reassignment recomputes narrowing. Lambda captures keep creation-site narrowing. Narrowing never skips runtime liveness checks.
- Member variables do not narrow and must be read into a local first.
- There is no truthiness: conditions and operands of `not`, `!`, `and` and `or` must be `bool`. A non-`bool` `if x:` / `if not x:` is a compile error. Its quick fix writes `x != None`, `x != 0`, `x != ""` and so on.

### 5.5 Conditional expressions

- Only `a if c else b`, where `c` must be `bool`.

### 5.6 match

- Value patterns are literals or constant expressions of the matched value's static type; any other type is a compile error. A `Variant` value takes patterns of any type.
- `var x`, `_`, array and dictionary patterns with `..`, comma-separated patterns and `when` guards are as in GDScript.
- New patterns:
  - enum variants and structs: `Command.MOVE(target = pattern, ..)`, `Hit(damage = var d, ..)`; `var field` is short for `field = var field`, unlisted fields require `..`, and patterns nest;
  - built-in constructors: positional `Ok(var v)`, `Err(var e)`, `Some(var x)`, `Finished(var v)`, `Cancelled(var r)` without type prefixes;
  - `Option` also matches `None` and `var x` narrowed to `T` (`None` paired with `Some(var x)` or `var x` is exhaustive).

  ```gdscript
  match cmd:
      Command.IDLE:
          stop()
      Command.MOVE(var target):
          walk_to(target)
      Command.ATTACK(target = var enemy, power = 0):
          taunt(enemy)
      Command.ATTACK(var target, ..) when target.is_in_group("bosses"):
          flee()
      Command.ATTACK(..):
          hit()
  ```

- A `match` over an enum of this language, `Result` or `Option` must be exhaustive or include `_`. One over an open engine enum must include `_`. `when` branches do not count toward coverage. Other types need not be exhaustive. An exhaustive `match` over an enum of this language covers all return paths. An invalid integer injected from the engine raises a program error at the `match`.
- [Known limitation] No type patterns (use `if x is T`), no `match` expressions, and no bindings in comma-separated patterns in the first version.

### 5.7 Loops

- An optional loop-variable type annotation must be the element type or a supertype. Iterating over a `float` or vector is a compile error, offering `range(...)`.

## 6. Initialization and lifecycle

### 6.1 Member initialization

- `_init` signatures are unconstrained by override rules.
- Instance and static member initializers write the backing field directly in declaration order, bypassing setters. A member with both gets a hint. Directly reading a later-declared member of the same class is a compile error. Derived state belongs in `_ready` or `_static_init`.

### 6.2 Must-assign members

- `_init` on `Node` and `Resource` scripts takes no parameters; constructor inputs are expressed as must-assign members (non-null without defaults).
- A `Node`'s must-assign member must be assigned before its scene finishes becoming ready, by one of:
  - **the scene**: exported members supplied in the scene;
  - **the creator**: `instantiate(...)` and `new(...)` take named member values and assign them before returning, e.g. `BulletScene.instantiate(speed = 300.0, target = enemy)`. Omitting a member that the scene resource rules require in `PackedScene[T].instantiate(...)` is a compile error. Omitting one in `new(...)` is checked at runtime;
  - **an ancestor**: an ancestor in the same scene assigns it in `_ready`. It does so via `$Child.target = v` or an `@onready` reference visible to the scene model. Invisible dynamic setup must use `T?`.
- **When checks run**:
  - scene- or creator-supplied members are checked when the node becomes ready, before `@onready` initializers;
  - ancestor-assigned members are checked once the whole scene is ready. Reading one in the child's own `_ready` is a compile error;
  - reading an unassigned member before ready is a program error naming the member.
- `Resource` has no ready phase: `.tres` files are checked statically, and `new()` instances are checked on read.
- When a script's `_init` has required parameters, attaching it without arguments, such as via `set_script`, is a program error. `_init` is then skipped.
- Autoloads outlive scenes, so scene-node references on them must be `T?`. [Known limitation] There are no post-ready non-null members in the first version.

### 6.3 `@onready`

- Combining `@onready` with `@export` is a compile error. So are awaiting in an initializer and reading an `@onready` member directly in `_init` or `_enter_tree`. Reading an uninitialized `@onready` member indirectly through a call is a program error naming the member.

### 6.4 Ready order

- If a child's `_ready` reads an uninitialized member of its parent, a program error explains the cause. This is not a blanket compile error, because runtime attachment under an already-ready parent is valid. The error suggests one of two fixes:
  - configuring the child from the parent's `_ready`;
  - making the child's `_ready` `async` with `@parent(T)` and `await get_parent().ready`.

### 6.5 Setters before ready

- Setters run immediately. Accessing an `@onready` member or child before ready is a program error naming it. A node-syncing setter stores the value, syncs guarded by `is_node_ready()`, and syncs once in `_ready`. `await ready` in a setter is a compile error.
- Reading a backing-field-less property whose getter reads children before ready is a program error naming the unready node. Unwritten properties do not run their setter at ready. Exported properties whose default bypasses a node-syncing setter get a hint.

### 6.6 Destruction

- During `PREDELETE`, a `RefCounted` instance may read and write its own fields and call its own methods normally. This takes no new reference. In the `PREDELETE` branch, using `self` as a value is a compile error. That covers use as an argument, storing, returning, comparing and so on, other than `self.x` and `self.f()`. Using `await` there is a compile error too. Meanwhile, `weak` references to `self` yield `None`.

### 6.7 Reference cycles and leaks

- Evident cycles get a warning with a `weak` quick fix:
  - a closure capturing `self` stored in `self`'s member, signal connection or container;
  - two `RefCounted` classes holding strong references to each other.
- A warning flags a `Node` from `new()` / `instantiate()` that some path neither parents, returns, stores nor frees. It also flags a `remove_child` result neither stored nor freed.
- Debug builds list leaked nodes and objects with their creation sites on exit.

## 7. Scene model

### 7.1 Typing nodes

- The compiler collects every scene structure using a script, including inherited scenes and instance overrides. It uses them to type `$X`, `%X` and literal-path `get_node`.
- Conflicting types across scene structures are a compile error. Assign the path to a variable annotated with a common base class or interface, e.g. `@onready var weapon: Node3D = $Weapon`.
- A missing path is a compile error marked on the scene lacking the node. The next section defines which scenes require it.

### 7.2 Structures that differ between scenes

- **Child nodes required by a base class and provided by inheriting scenes**: checked by scene usage, as for required exports. A base-only scene may lack the node. A root or instanced scene must have it. An unreferenced scene gets only a warning. `$X` takes its type from the scenes that have it.
- **Optional child nodes**: literal-path `get_node_or_null(...)` has type `T?`, and scenes lacking the node are valid. When `$X` is missing in a used scene, a quick fix rewrites it to `get_node_or_null`:

  ```gdscript
  @onready var trail_right = get_node_or_null("Container/TrailRight")   # GPUParticles3D?
  ```

- **Child nodes added at runtime**: not typed by path. `$X` absent from every scene, unreferenced scenes included, is a compile error. Its quick fix suggests storing the node in a typed member on `add_child`, e.g. `var tetromino: Tetromino?`. Dynamic lookup by name uses `get_node_or_null("X") -> Node?` with `as`. A literal `get_node_or_null` path absent from every scene gives a warning. When the node is genuinely added at runtime, `@warning_ignore` silences it.

### 7.3 Upward access

- Autoloads are accessed by global name, typed as their script's class. A literal `/root/X` is a compile error whose quick fix writes `X`.
- Class annotations `@parent(T)` and `@owner(T)` give `get_parent()`, `$".."` and `owner` type `T`. Without them, `get_parent()` and `owner` are `Node?`, while `$".."` and `get_node("..")` are `Node`. Then `get_parent().x` or `owner.x` is a compile error. Its quick fix infers a common type from scenes to add `@parent(T)` / `@owner(T)`, or appends `.unwrap()`:

  ```gdscript
  @owner(Combat.Combatant)
  class_name HealthComponent extends Node

  func _ready():
      current_health = owner.init_health
  ```

- **Static check**: checked on scenes placing the script. `@parent(T)` checks the scene parent. When the scene root is instanced into another scene, it checks the outer parent. On the main scene, it checks the `Window` root viewport. Mismatches are marked on the scene. Other root scenes check at runtime. `owner` is the containing scene's root, or the outer scene's root when instanced. A root-used scene has no `owner` on its root. So attaching an `@owner` script there is a compile error marked on the scene.
- **Runtime**: `@parent(T)` checks on parenting (`NOTIFICATION_PARENTED`). Calling `get_parent()` without a parent is a program error. `@owner(T)` checks on every read of `owner` that it is non-null and matches `T`. A failure is a program error naming the requirement and the actual `owner`. Moving nodes or calling `set_owner()` does not check.
- `get_owner()` and `get_node_or_null("..")` always have type `Node?` for testing presence when a requirement is declared.

### 7.4 Scene resources

- `preload` and literal-path `load` are typed by actual resource contents. Scenes yield `PackedScene[T]`, with `T` the root node's type or script class. `preload` on a script yields its class.
- Exported scenes use `PackedScene[T]`, checked by the editor, and `instantiate()` yields `T`. `PackedScene[T]` is covariant in `T`.
- Required `instantiate(...)` arguments depend on whether the scene is known statically:
  - statically known (`preload`, literal-path `load`, and constants initialized with them): must pass must-assign members not supplied by that scene;
  - other `PackedScene[T]` values (exports, variables, parameters, returns): must pass non-exported must-assign members of `T` (exported ones come from the scene);
  - a scene may be converted to the second kind by filling an export, assigning or passing it. Its root's actual class may then leave unprovided only `T`'s non-exported must-assign members. Otherwise the conversion is a compile error, marked on the scene filling the export or at the assignment.
- Runtime-path `load` yields `Resource`, with nullability as for engine APIs. Its scene's `instantiate()` yields `Node`, requiring explicit conversion.

### 7.5 Required exports

- Non-null object exports must be assigned; optional ones use `T?`.
- Checked by scene usage:
  - instanced or inherited scenes accept assignment at any level of the instance or inheritance chain. Each instance is checked separately, with missing values marked on its scene;
  - root-used scenes (main scene, literal-path `load`/`preload`, exported `PackedScene[T]` values) check the scene itself. Values passed at `instantiate(...)` count, and omitting them there is a compile error;
  - unreferenced scenes get only a warning.
- Three check points:
  - **Editing**: clearing is allowed, marking the Inspector field and scene-tree node immediately without blocking edits until reassigned;
  - **Running**: scene missing-value errors do not block running in the editor. A still-missing value raises a program error at ready, before `@onready` initializers, naming the member;
  - **Exporting**: scenes certain to miss a value report export-plugin errors. The export dialog then shows a failure naming the scene and member. Warnings pass. CI blocks via the headless analysis command. [Known limitation] The engine exporter still writes output and exits 0 on plugin errors.
- Remote-inspector `null` writes to non-null members fail the entry check, a program error. The write is rejected and the old value kept.

### 7.6 Shared resources

- Resource-sharing semantics are unchanged. The compiler tracks cached resources (`load`, `preload`, exported properties) and embedded sub-resources not marked local to scene.
- Mutating a property or calling a mutating method on a known-shared resource gives a warning. It lists the sharing sites and offers two quick fixes: mark local to scene, or `duplicate()` before mutating. When the sharing is intended, `@warning_ignore` silences it. Runtime-replaced resources are untracked.
- `duplicate()` preserves the receiver's static type; copy depth uses `duplicate_deep(mode)`.
- Reference cycles across `preload`, exported `PackedScene`s and resources are compile errors whose quick fix rewrites one edge to runtime `load`.

### 7.7 Connections and paths in scenes

- Stored scene signal connections check target method existence and parameter compatibility.
- Animation property tracks, method tracks and `"position:x"` property paths resolve as static references. They participate in rename and find references.

## 8. Errors

### 8.1 Program errors

- A program error aborts the entire call chain of this language back to the engine entry point. Calls to methods declared in this language share the chain, whatever the receiver's class and even through an interface. So do `f.call(x)` and `f.callv(args)` on lambdas or method values, including after `bind()`. Only engine entry points start a new chain, such as signals, engine `Callable`s and engine virtuals. Unwinding releases held references and logs the error and call stack. When running in the editor, it breaks in the debugger. The game keeps running.
- Except for `assert`, debug and release builds detect and handle program errors identically. Debug builds only enrich diagnostics and report leaks on exit.
- Engine entry points requiring a return value receive the default value of that type.
- Every "program error" in this specification follows this rule. Examples include freed-object access, failed `unwrap`/`expect`, out-of-range indexes and integer division by zero. They also include reading unassigned members before ready, entry-check failures, mutating `Dictionary` keys and awaiting cancelled tasks.
- Nesting calls of this language past 1024 on one thread is a program error rather than a stack-overflow crash.

### 8.2 Expected failures

- Expected failures use `Option` (ignorable) or `Result` (non-discardable).
- The sole special syntax is short-circuit postfix `?`:
  - on `Result[T, E]`: on `Err(e)`, returns `Err(e)` from the current function, or else yields `T`. The current function must return `Result[_, E]` with the same `E`; `map_err` converts a differing `E`;
  - on `T?`: returns `None` from the current function on `None`, or yields `T`; the function must return `_?`;
  - using `?` in a `void` function is a compile error, as is propagating directly between `Option` and `Result`. Convert with `ok()` and `ok_or()`;
  - precedence parses `await f()?` as `(await f())?`.

  ```gdscript
  func load_profile(path: String) -> Result[Profile, Error]:
      var text = read_text(path)?
      return Ok(parse_profile(text)?)
  ```

### 8.3 Assertions

- `assert(condition, message)` runs only in debug builds, meaning editor runs and debug exports. It raises a program error on failure. Release builds strip it without evaluating condition or message.
- The message is `String`, evaluated only when the condition is false. `await` or postfix `?` in condition or message is a compile error. The condition must be `bool`. For example, `assert(save_data, "...")` is a compile error offering `save_data != None`. Release-active checks use `expect` or `if`.

### 8.4 Diagnostics

- Compile errors, warnings and hints come from the analysis service. They appear in source, the Inspector and the scene tree.
- Program-error diagnostics report location, relevant members or objects, and call stack. They include the details sections specify, such as must-assign member names, free sites, and `owner` requirements vs actual `owner`.

## 9. Coroutines

### 9.1 async and Task

- Functions and lambdas containing `await` must write `async func` in their signature, e.g. `button.pressed.connect(async func(): await fade_out())`. Omitting `async` is a compile error with a quick fix adding it.
- Only `void`-returning engine virtuals may be `async`. `async func _init()` and `async` overrides of non-`void` engine virtuals are compile errors. This holds also when the return type is omitted. Engine-invoked positions accept `async` functions and run them as root tasks.
- Calling an `async` function starts it immediately, running until its first suspension, and returns `Task[T]`. A `void` function returns `Task[void]`; `void` is a valid type argument only on `Task`. Bare `Task` is a compile error. `await f()` and `await t` yield `T`.
- Discarding an `async` call result is a compile error; write `f().detach()` to ignore it deliberately.
- Setters, getters, `@onready` initializers and static initialization cannot `await`.

### 9.2 How tasks end

- A task ends in one of three states. **Finished** means it returned a value. **Cancelled** means `t.cancel()` was called, its owning object was freed, or the source of `Task.of(signal)` was freed. **Failed** means a program error occurred inside the task, including awaiting a cancelled task or freed signal source.
- Awaiting a cancelled task is a program error. Failures propagate up the `await` chain to the root task, which is `detach`ed or engine-called. They cannot be intercepted.
- `await t.settled()` treats cancellation as a value, returning `Settled[T]`: `Finished(v)` or `Cancelled(r)`. `r: CancelReason` is one of `REQUESTED`, `OWNER_FREED` and `AWAITED_FREED`. Failures propagate as usual.

### 9.3 Cancellation and ownership

- On a suspended task, `cancel()` immediately disconnects its awaited signal or task. Only code a running task runs synchronously can cancel it, such as a handler of a signal it emits. It then cancels at its next `await` or `return`, discarding its return value. Ended tasks are unaffected. After `cancel()`, the outcome is always cancelled.
- Cancellation follows ownership. A child task started and awaited directly as `await f(...)` belongs to its parent and cancels with it. Awaiting an existing `Task` value only stops waiting, while that task runs on.

### 9.4 Release

- Ending a task immediately releases its coroutine frame and held references without running code after the `await`. [Known limitation] There is no `finally` or `defer` in the first version.
- `Task` is a `RefCounted` result handle; a suspended coroutine does not keep `self` alive.

### 9.5 Combination and reentrancy

- `Task.of(signal)` wraps a signal. `Task.delay(seconds)` waits. `Task.any(list)` returns the first finished index and cancels remaining tasks by the ownership rules. `Task.all(list)` waits for all. Tasks started directly in `any`/`all` arguments are owned; existing tasks only stop being awaited.
- Reentrancy has no automatic policy; cancel old calls via their handle.

### 9.6 Awaiting signals

- A zero-parameter signal yields nothing. A single-parameter signal yields that parameter's type. Multi-parameter signals may be `await`ed only as statements, and using the result is a compile error.
- Signals accessed on a known class have known signatures. Known classes are those in ClassDB, declared in this language, or reflected from GDScript. Bare `Signal` expressions have unknown signatures. As statements they are unrestricted. Awaited as a value, they yield `Variant`: NIL for no arguments, the argument for one, an `Array` for several:

  ```gdscript
  var r = await DialogueManager.Resolved   # unknown signature: r is Variant
  if r is Array and r[0] == id:
      return r[1]
  ```

### 9.7 Freed objects and suspended coroutines

- When a coroutine's owning object receives `NOTIFICATION_PREDELETE` (before `Node` deletes children), its coroutines cancel silently and release frame references.
- An awaited object is another object's coroutine host or signal source. When it is freed before completion, judgment waits until the per-frame callback after the deletion queue flushes:
  - if the waiter's own object was also freed meanwhile, the waiter cancels silently (`OWNER_FREED`);
  - otherwise the waiter fails at `await` with a program error naming the freed object and free site. The error propagates up the `await` chain.
- `settled()` does not error, yielding `Cancelled(CancelReason.AWAITED_FREED)` for `Task.of(signal)` and `Cancelled(CancelReason.OWNER_FREED)` for a coroutine on another object:

  ```gdscript
  # hud.pet: HUD and player are both children of Main
  async func _ready():
      await player.died         # freed together on scene change: cancelled silently
      show_game_over()          # player freed alone without died: program error after frame
  ```

### 9.8 Cross-language await

- GDScript method calls may be `await`ed, waiting on `GDScriptFunctionState.completed` and entry-checking the declared return type. Without `await`, statement calls run past first suspension as if detached. Without `await`, value calls that receive `GDScriptFunctionState` raise a program error saying `await` is required.
- While awaiting `GDScriptFunctionState`, the target object is the call's receiver or `Callable` target. If it is freed or emits `script_changed`, the previous section's rules for freed awaited objects apply. `settled()` then yields `Cancelled(CancelReason.OWNER_FREED)`. [Known limitation] Reloading a GDScript script in the editor clears suspended functions without signals, leaving waiters suspended.
- When GDScript `await`s a coroutine of this language, first suspension returns `Task.completed`. It emits `completed(value)` on finish, and `completed(null)` plus `cancelled` on cancellation or failure. `async` methods report untyped `Variant` to the engine and show declared return types only in docs and hover.

## 10. Engine boundary

### 10.1 Types as the engine and GDScript see them

| This language | What the engine and GDScript see |
|---|---|
| Objects `T` and `T?` | That class in both cases; non-null enforced by entry checks |
| Nullable value types such as `int?` | Untyped `Variant` |
| `Array[T]` and `Dictionary[K, V]` with engine-representable elements | Engine typed containers (outer level only when nested) |
| Structs | `Dictionary` with `String` field-name keys |
| Payload enums | `Dictionary` with `String` field-name keys and reserved `"$variant"` variant name |
| `async` method return values | Untyped `Variant` |

- Through script reflection, GDScript statically checks global classes of this language. It checks instance members (properties, methods, signals) and positional `Player.new(...)` arguments. When `_init` is not `pub`, parameterized `new` is a GDScript compile error, and zero-argument `new` fails at runtime. Named must-assign arguments are specific to this language. Other class-level access is in the section on cross-language calls.
- **Entry checks**: values entering from the engine, GDScript, C# or the remote inspector are checked against declared types. This covers arguments, `set` writes, signal arguments, `await` results and decoding. Failures are program errors.
- `Option` encodes `None` as NIL. So `X?` crosses to the engine implicitly only when no `X` value encodes as NIL. When `X` is `Option` or `Variant`, it is a compile error offering `.flatten()` or `.unwrap_or(null)`.

### 10.2 Nullability of objects in engine APIs

- Engine APIs not audited in binding metadata assign object nullability by direction:
  - engine method returns and property reads are `T?`;
  - engine virtual-method and engine signal parameters are non-null `T`, and `null` fails the entry check. Overrides and handlers may write `T?` to accept `null`;
  - method arguments and property writes to the engine accept `None` (e.g. `set_owner(null)`).
- Audits proceed from the most-called APIs. Returns become non-null `T`, `T` that is null only when the engine reports a misuse, or `T?`. An example of the second is `get_tree()`, where `null` is a program error. Examples of the third are `get_collider()`, `get_first_node_in_group()` and `get_parent()`. Parameters are audited likewise, e.g. `Area3D.body_shape_entered` is marked nullable.

### 10.3 Binding metadata

- Unexported engine constraints are added in binding metadata alongside exported ClassDB metadata.
- `Variant`-bound math utilities return the shared argument type, e.g. `lerp`, `clamp`, `min`, `max`, `abs` and `wrap`. Mixing `int` and `float` is a compile error whose quick fix writes `float` (e.g. `min(1.5, 1.0)`).
- Engine APIs filtered by a literal class name take that class's type (e.g. `find_children("*", "MeshInstance3D") -> Array[MeshInstance3D]`).
- Null-on-failure engine APIs (such as `FileAccess.open`) get `Result`-returning standard-library wrappers.

### 10.4 Member names as strings

- On known target types, literal names in the following positions resolve as static references. The positions are in `connect`, `emit_signal`, `call`, `call_deferred`, `set_deferred`, `has_method`, `Callable(obj, "m")`, `tween_property` and `"position:x"` property paths, marked in binding metadata. Such names are checked for existence and value type, and included in rename and find references. Non-literal names are explicit dynamic accesses.

### 10.5 Members exposed through reflection

- Reflection by name (`get_property_list`, `has_method`, `call`, `get`, `set`) exposes only `pub` members and members the engine must find by name. The latter are exports, signals, engine virtual overrides, scene-connected methods and compiler-collected literal member names. Other members are invisible by name: a GDScript compile error, or a missing-member error at runtime.
- `pub` members typed as containers of language-only types are omitted from reflection. `struct` members are exposed with copy encode/decode.
- Method-value `Callable`s (e.g. `tween_callback(change_weapon)`) bind directly without name lookup. `pub` inner classes are exposed as script-valued class constants. The debugger sees all members.

### 10.6 Numeric boundaries

- Narrow or unsigned engine integer parameters and properties are range-checked: `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32`, `uint64` and `char32`. Out-of-bounds values raise a program error. [Known limitation] Classes from other extensions are absent from the bundled engine API description and unchecked.
- Writes to 32-bit engine float slots (such as standard-build `real_t` vector components) narrow implicitly.

### 10.7 String and StringName

- Runtime conversions between `String` and `StringName` or `NodePath` on hot paths get a performance hint.

### 10.8 JSON

- Decoding JSON numbers to `int` accepts exact integer `float`s within `int64` and errors otherwise without truncating.
- The standard library encodes and decodes structs and enums with JSON, `var_to_str` and `ConfigFile`. Containers of structs or payload enums are first converted via `Array(hits)` / `Dictionary(d)`.

## 11. Coexisting with GDScript and C#

### 11.1 The static view of GDScript classes

- This language reflects GDScript classes and autoloads, honoring GDScript subclassing in parameters, `is` and `as`. Object-typed members, parameters and returns have type `T?`. Value types keep their declared types, and untyped ones are `Variant`.
- Autoload names are global identifiers typed as their script's class (including scripts without `class_name`).
- A GDScript class named via `class_name` or `preload` is constructed with `new(...)` typed from its `_init`. It exposes static methods and value-typed constants on the class (`Unit.make()`, `Unit.TILE_SIZE`).

### 11.2 Inheritance

- Cross-language inheritance is unsupported in both directions:
  - classes of this language cannot extend GDScript classes. When a plugin requires inheritance, write a thin GDScript forwarding subclass. Its calls back into instance members remain statically checked in GDScript;
  - a GDScript script extending a class of this language gets an explanatory error from the analysis service;
  - migrating a class requires migrating its GDScript subclasses with it.

### 11.3 Cross-language calls and messages

- GDScript and C# can access by name only the members reflection exposes.
- [Known limitation] Beyond instance access and `new`, GDScript can read and write `pub` static variables. It can also read constant values and `Player.Inner` scripts. It lacks class-level support for the following. Instead, forward through a method of an instance or an autoload:
  - calling static methods of this language;
  - using its enums, constants or inner classes in GDScript type annotations or constant expressions;
  - typing autoloads written in this language, which GDScript sees as `Node` or untyped.
- Messages with C# use structs encoded as field-name `Dictionary` values and entry-checked.

## 12. Runtime

### 12.2 Performance

- Clearly faster than GDScript; C# is not the target.
- [Open] When native code is generated: at runtime, at export, or both.

### 12.3 Threads

- The VM runs safely on any engine-chosen thread, such as background resource loading and physics threads. Data races on shared objects are the user's responsibility.
- [Known limitation] The first version adds no threading API, using the engine's `Thread`, `WorkerThreadPool` and `Mutex`.

### 12.4 Hot reload

- Function-body-only edits swap in while running. Future calls run new code. Suspended coroutines and previously created lambdas finish on the version that created them.
- Member, signature, constant or structural changes do not migrate state and request a restart. The running game refuses this and all subsequent reloads until restart. This ensures each class has one version per run.

### 12.5 Debugging support

- Integrates with the engine debugger (breakpoints, stepping, locals, call stacks).
- Debug builds record free sites of instances of this language and of objects freed by it. They list leaked nodes and objects on exit.

## 13. Toolchain

### 13.1 Analysis service

- External IDEs connect to the analysis service via LSP, and CLI/CI runs the same analysis headlessly. Headless analysis exits non-zero on any compile error, scene missing-value errors included.

### 13.2 Tools in the first version

- **Diagnostics and navigation**: live diagnostics shared across editor, LSP and CLI. Completion, hover types, go to definition, and find references across code and scenes.
- **Scenes and refactoring**: scene-aware rename. Scene checks for `$X`, signal connections, `PackedScene[T]`, `@parent`/`@owner` and required exports. Quick fixes inserting inferred types, completing `match` arms, adding conversions and applying rewrites throughout this spec.
- **Formatting**: a single optionless rule set. It preserves the syntax tree, comments and literal spellings, is idempotent, and skips files with syntax errors:
  - one tab per indentation level. Comments indent with their block while keeping extra levels. Comments before a block's first line belong to that block. At multi-block endings they follow their original column, never shallower than a following comment's block;
  - bracket continuations indent one level past the opening bracket's line, with standalone closing brackets returning to it. `\` continuations indent one level past the statement's first line. Lambda bodies inside brackets indent one level past the line ending their header;
  - one space surrounds binary operators, including assignments, named-argument `=`, `:=` and `->`. One space follows `,`, `;` and `:`, with none before. No spaces go inside brackets, after unary operators, before call/subscript brackets, or around `.`. Nor do they go before `?`, inside `$` paths, or inside single-line dictionary braces. Two spaces precede trailing comments. Extra spaces and trailing whitespace are stripped;
  - top-level functions and classes, structs and interfaces included, have two blank lines around them. Those in inner classes have one. Attached comments and annotations count as part of them. The rule does not apply before a block's first member or to bodyless methods. Other blank-line runs collapse to one, with no leading blank lines and a single trailing newline;
  - line breaks stay put: no width-based splitting or joining, and `;`, single-line blocks and `\` remain. Literals, quotes, parentheses, comment text, trailing commas and line-ending style are preserved.
- **Lints, debugging and testing**: lints report as analysis warnings suppressed by `@warning_ignore`, with hints on unused ignores. Engine debugger integration. A built-in headless-capable test runner. There is no package manager.

### 13.3 Refactoring

- Refactorings include their scene half. Mechanical, meaning-preserving edits are previewed, applied together and undoable. They cover export keys, signal method names, animation track paths and lossless value conversions. Judgment-dependent edits, such as node type changes and incompatible resources, are only reported with options.
- Renaming or moving a node in the scene editor updates `$X`, `%X` and literal `get_node` paths. The updates join the same undo step without preview and are logged in the output panel. Non-literal paths and paths resolving elsewhere in other scenes are only reported with quick fixes.

### 13.4 Migration

- Migration is guided by compile errors and quick fixes, such as adding `pub`, parameter types, truthiness rewrites and `.detach()`. Migrating a class lists its GDScript subclasses with an option to migrate them together.

## 14. First-version scope and open questions

### 14.2 Compatible later additions

These features can be added compatibly after the first version. Full designs for generics and pre-ready suspending setters are in [design-notes.md](design-notes.md):

- **Language features**:
  - user-defined generics and generic interfaces;
  - `@tool`;
  - aliases/`import` and cross-class `preload` access;
  - stricter `private`;
  - positional struct construction, mutating methods, operator overrides and indirect recursion;
  - `@export` for structs and payload enums;
  - function-type parameter names, direct `f(x)` and shared captures;
  - ordinary functions where `async` is expected;
  - call-site spread of rest parameters;
  - field narrowing / conditional binding;
  - `match` type patterns, `match` expressions and multi-pattern bindings;
  - post-ready non-null members;
  - auto-suspending pre-ready setters;
  - always-on assertions;
  - reentrancy annotations, multi-arg signal destructuring and typed `Signal`.
- **Interop and tooling**:
  - optional compiler-carrying runtime library;
  - `Task` handles for GDScript coroutines;
  - GDScript enums/inner classes/static variables and static-method calls;
  - advanced refactorings, deeper profiler integration, doc generation and forwarding shells.

### 14.3 Open questions

- When native code is generated: at runtime, at export, or both.
