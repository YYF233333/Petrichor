# Getting Started

中文：[getting-started.zh.md](getting-started.zh.md)

This guide covers installing Petrichor, upgrading between builds, editing in VS Code, running the `pet` command-line tool, migrating from GDScript, and building from source. For the language overview, design goals, and roadmap, see the [README](../README.md).

## Requirements & Supported Platforms

Petrichor requires the official Godot 4.7 editor, standard or .NET.

Every release is tested in CI with the official Godot editor and official export templates, covering editing, running, and exporting both debug and release builds.

| Platform | Library | Tested on |
|---|---|---|
| Windows | x86_64 | Windows Server 2022 |
| Linux | x86_64 (requires glibc 2.35+) | Ubuntu 22.04 |
| macOS | universal (x86_64 and arm64; requires macOS 11+) | macOS 15 on arm64; x86_64 via Rosetta |

## Installing

Download one of the packages from the [Releases](https://github.com/YYF233333/Petrichor/releases) page. The same page also includes the optional VS Code extension.

- **Demo (`petrichor-<version>-demo.zip`)**: Kenney's FPS starter kit with migrated scripts. Unpack it, open the project in Godot, and run it once import finishes.
- **Extension (`petrichor-<version>-addon.zip`)**: Unpack it into your project root to create `addons/petrichor/`. Restart the editor, then create `.pet` scripts as usual.

### macOS quarantine

On macOS, the shipped binaries are ad-hoc signed and not notarized. Because of that, macOS quarantines browser downloads and blocks them from loading. After unpacking, clear the quarantine attribute from your project directory:

```sh
xattr -dr com.apple.quarantine addons/petrichor
```

## Upgrading

Close the Godot editor, then replace `addons/petrichor/` with the files from the new release. Run the project once from the editor afterward so every script recompiles against the new build.

## Editing in VS Code

Download `petrichor-<version>.vsix` from the [Releases](https://github.com/YYF233333/Petrichor/releases) page and install it via **Extensions: Install from VSIX…**. Open your project in Godot first, then open the same folder in VS Code.

VS Code connects to that Godot editor for diagnostics and language tools, except breakpoints. Saving a script in VS Code hot reloads a running game the same way saving in Godot does.

Keep three workflow details in mind:

- Type checking reads scenes and classes through Godot, so without a running editor you only get syntax highlighting. When a `.pet` file is active, the status bar shows whether VS Code is connected.
- After undoing a rename in VS Code, save the modified files manually. VS Code auto-saves after a rename, not after undoing one.
- When Godot is in the background, it sleeps 100 ms per frame, which can delay completion and other requests by up to that amount. Lowering **Interface → Editor → Timers → Unfocused Low Processor Mode Sleep (µsec)** reduces that delay at the cost of higher power usage.

## Command-Line Tool (pet)

Run `pet` from the project root to check, build, format or migrate the entire project, or to bridge LSP:

```sh
addons/petrichor/pet check            # check files on disk without writing bytecode
addons/petrichor/pet build            # check files and write bytecode
addons/petrichor/pet format --check   # list unformatted scripts; omit --check to format in place
addons/petrichor/pet migrate          # convert GDScript scripts to Petrichor; see "Migrating from GDScript" below
addons/petrichor/pet lsp              # bridge stdio for LSP clients such as Neovim and Helix
```

On Windows, use `addons\petrichor\pet`.

If the Godot editor already has the project open, `pet` runs inside that editor without spawning another process. Otherwise, it launches a headless copy of the Godot binary last recorded by the editor. You can also pass `--godot <path>` explicitly, for example in CI:

```sh
addons/petrichor/pet build --godot <godot>
addons/petrichor/pet format --check --godot <godot>
```

Diagnostics are printed one per line (`player.pet:12:5: error: ...`). Exit codes are `0` for success, `1` for errors or unformatted scripts, and `2` if the command could not run. Because `pet check` inspects files directly on disk, AI coding agents can run it right after editing files to verify their changes.

## Migrating from GDScript

`pet migrate` converts a project's GDScript scripts to Petrichor. It renames each `.gd` file to `.pet`, writes the types it can infer from your code and scenes, and updates the paths that point to those scripts. Where it cannot be sure, it does not guess: it leaves a compile error for you to resolve with the editor's quick fixes. If you later decide to leave, `pet migrate --reverse` converts the code back to GDScript.

```sh
addons/petrichor/pet migrate [--reverse] [--dry-run] [--allow-dirty] [<path>...]
```

A migration takes four steps:

1. Commit the project to git, then save and close the scripts you are migrating.
2. Do a dry run and read the report.
3. Run the migration.
4. Work through the compile errors it leaves.

### Step 1: Prepare

A migration renames scripts and rewrites paths in scenes, resources and `project.godot`. Commit the project to git first, so that you can restore everything if you do not like the result.

The tool refuses to run if any file it would change has uncommitted changes or is not tracked by git, and it lists those files. To skip this check, pass `--allow-dirty`; you also need it when the project is not in a git repository.

Next, save any unsaved changes in Godot and other editors, and close the scripts you are migrating in Godot's script editor. Otherwise the tool again writes nothing and lists the files that are in the way. Godot itself can stay open; it reloads the project after the migration.

### Step 2: Dry run

With `--dry-run`, the tool prints its report and writes no files:

```sh
addons/petrichor/pet migrate --dry-run
```

Here is the report for Kenney's FPS starter kit, cut where `...` appears:

```text
objects/enemy.gd → objects/enemy.pet
objects/player.gd → objects/player.pet
...
8 × Write the type
2 slot(s) received both int and float, which a use tells apart.
objects/enemy.pet:30:17: hint: "amount" is left without a type, as no type fits all the values it receives: int (res://objects/enemy.pet:68), float (res://objects/player.pet:205).
...
5 × Make it pub
5 × Write .unwrap()
...
objects/enemy.pet:30:17: error: Parameter "amount" needs a type.
...
petrichor migrate: 7 script(s), 0 added, 0 left out, 8 type(s), 3 withdrawn, 16 fix(es), 9 path(s) renamed, 7 error(s), dry run
```

The report has five kinds of lines, followed by a last line of totals:

| Line | Meaning |
|---|---|
| `a.gd → a.pet` | The file will be migrated. A line ending in `added:` is a file you did not name that has to migrate too, with the reason |
| `a.gd: left out: …` | The file stays in GDScript, for the reason given |
| `N × …` | One kind of change the tool made and how many times, such as writing a type or adding `pub` |
| `hint:` | A place for you to review |
| `error:` | A compile error left after the migration, in the format of `pet check` |

Read every `hint:` line. There are four kinds:

- a variable or parameter that got no type, with the type and location of each value it receives;
- a type inferred from incomplete evidence, for example only from calls by name;
- a member still accessed dynamically by name. GDScript works this way, so the tool keeps the code as it is, and the compiler cannot check it;
- a conversion written as `x as T`. It gives `None` when `x` is not a `T`, where a debug build of GDScript stops with an error.

Take `amount` in the report above. `damage(amount)` receives an `int` from the enemy and a `float` from the player. Subtracting it from an integer health gives different results for the two types, so the tool cannot choose for you. It writes no type, and leaves a `hint:` with the reason and an `error:` for you to settle. The line `2 slot(s) received both int and float` counts places like this one.

### Step 3: Migrate

If the report holds no surprises, run the command again without `--dry-run`. The tool writes all the files or none of them.

```sh
addons/petrichor/pet migrate                       # migrate the whole project
addons/petrichor/pet migrate objects/enemy.gd ui   # migrate only these files and folders
```

Paths are relative to the project root. When you migrate part of a project, some other files have to migrate with it; see "Migrating part of a project" below.

The tool updates the script paths written in scripts, text scenes (`.tscn`), text resources (`.tres`) and `project.godot`. It does not change three kinds of paths:

- paths in binary scenes and resources (`.scn`, `.res`). These still find the scripts by uid, and the report lists each with a `warning:`;
- paths in C# code;
- paths built at run time, such as `"res://levels/" + name + ".gd"`.

Compile errors left in the code are not a failure, and neither is a file the report leaves out: the exit code is `0`. When the tool refuses to run or a write fails, it writes nothing and exits with `1`. It also exits with `1` when a path you pass contains nothing to migrate. A dry run exits with the code the real run would give, so a dry run that exits with `0` means the migration will not be refused.

### Step 4: Fix the compile errors

Wherever a change needs your judgment, the tool leaves a compile error. Open the project in Godot, or run `pet check`, to work through them. Settling one error can bring up the next: once you write `amount: float`, the compiler also checks `health -= amount`, and asks for an explicit conversion there.

In Godot's script editor, click the light bulb beside an error line, press Alt+Enter, or choose **Quick Fix...** from the context menu to list the fixes for that place. When a script has two or more errors with the same automatic fix, **Fix all '…' in file** applies them at once. VS Code offers the same fixes as code actions.

The tool writes three things around null that are worth reviewing afterward:

- **`.unwrap()`**: the original code used this value without checking it for null. If the value is null, the program fails at the `.unwrap()`, which may be earlier than where GDScript fails. Where null is a real possibility, replace it with a check.
- **`T?`**: the value can be null, for example an export that no scene sets, a member with no initial value, or a variable that is assigned `null`.
- **`@late T`**: when a member is assigned only after creation and every read would need `.unwrap()`, the tool uses `@late` instead, and the reads need no `.unwrap()`. Reading the member before it is assigned still fails at the read.

The tool does not reformat files; run `pet format` when you are done.

### What stays in GDScript

Even when you migrate the whole project, these files stay in GDScript, each with a `left out:` line in the report:

- a script with `@tool`, together with its whole inheritance tree. Support for `@tool` arrives in 0.4;
- plugins under `addons/`, one line per plugin. To migrate a plugin, pass a path inside its folder;
- script templates, the files the editor creates new scripts from. They live in the folder named by the `editor/script/templates_search_path` project setting, `res://script_templates` by default;
- a script that has a `.pet` file of the same name in its folder, together with its whole inheritance tree;
- a script that any of the files above uses directly through its class name.

The last rule comes from GDScript's side. GDScript can create instances of a Petrichor class and call their methods. It cannot call the class's static methods or read its enum values through the class name. Nor can it use the class's enums, constants or inner classes as types. So when a GDScript file that uses a class in these ways stays, the class has to stay too.

### Migrating part of a project

Pass files or folders to `pet migrate` to migrate only those. The tool adds the files that have to migrate with them and marks each with `added:` in the report:

- scripts in the same inheritance tree, because neither language can extend the other's classes;
- GDScript files that use these classes directly through their class names, for the reason given in the previous section.

For example, migrating only `boss.gd` also migrates its base class, reported as `enemy.gd → enemy.pet, added: in the inheritance tree of res://boss.gd`. The tool reads the remaining GDScript files without changing them, except to update their paths to the renamed scripts.

### Converting back

`pet migrate --reverse` converts `.pet` files back to `.gd`. It works like a migration: commit to git first, do a dry run if you like, and pass paths to convert only part of the project.

Converting back is meant for Petrichor code written after a migration, for example when a team leaves after a trial. If you only want to undo a migration you just ran, restoring with git is simpler.

The tool removes what GDScript has no syntax for, such as `pub`, `async`, `@late`, `.unwrap()` and the `?` after object types. It writes each type only as far as GDScript can express it.

Two kinds of files cannot be converted, and they stay `.pet` files together with their whole inheritance trees:

- a file with compile errors;
- a file that uses a feature GDScript lacks, such as `struct`, payload enums, interfaces or `Result`.

The report gives such a file a `left out:` line and lists each place with a `hint:`, such as `hint: GDScript has no struct.` Leaving a file out is not a failure: the command exits with `0` even when no file can be converted.

When you convert back part of a project, the tool again adds the files that have to go along. These are the inheritance trees of the files you pass, and the trees of the `.pet` classes that those files use directly through their class names. The `.pet` files that stay now face GDScript classes, so the tool checks them again and lists the errors this causes. For example, an object member of a converted class now reads as `T?`. These errors do not stop the write; they are left for you to fix. GDScript files that call the converted code are not checked, and Godot reports any problems when it loads them.

Before writing, the tool parses each converted file again and compares it with the original `.pet` file. A difference means a defect in the converter. In that case it writes nothing, exits with `1`, and asks you to report the output in [Issues](https://github.com/YYF233333/Petrichor/issues). A dry run makes the same comparison and exits the same way.

## Building from Source

Building from source requires a C++17 toolchain and SCons:

```sh
git submodule update --init
scons
```

See the [development notes](development.md) (Chinese) for build options, test commands, benchmarks, debugging tips, and known pitfalls.

## Next Steps

- Read the [language specification](language-spec.md) for the full rule set.
- File defects in [Issues](https://github.com/YYF233333/Petrichor/issues) and share language feedback in [Discussions](https://github.com/YYF233333/Petrichor/discussions); both receive a reply within 48 hours.
