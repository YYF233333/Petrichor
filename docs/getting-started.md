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

Run `pet` from the project root to check, build, format, or bridge LSP for the entire project:

```sh
addons/petrichor/pet check            # check files on disk without writing bytecode
addons/petrichor/pet build            # check files and write bytecode
addons/petrichor/pet format --check   # list unformatted scripts; omit --check to format in place
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

`pet migrate` converts GDScript files to Petrichor, and `pet migrate --reverse` converts `.pet` files back to GDScript. Pass files or folders, relative to the project root, to convert only those; with no path, the whole project converts.

### Before you start

Commit the project to git. The tool writes only files that git can give back: it refuses to run when a file it would write has uncommitted or staged changes, or is untracked or ignored, and when the project is not in a git repository. Those files are the scripts it renames with their `.uid` files, and the scenes, resources, scripts and `project.godot` whose paths it rewrites. The report lists each refused file with the reason, and `--allow-dirty` skips the check.

Also save your changes in Godot and any other editor, and close the scripts being migrated in Godot's script editor. Otherwise the tool writes nothing and lists the files in the way.

### Running the migration

```sh
addons/petrichor/pet migrate --dry-run   # print the report; write nothing and skip the git check
addons/petrichor/pet migrate             # migrate and print the report
```

The tool renames each `.gd` file to `.pet`, along with its `.uid` file, and updates every path that names them. It writes all files or none. Godot can stay open, and it reloads the project afterward.

This shortened report comes from a dry run on Kenney's FPS starter kit:

```text
objects/enemy.gd → objects/enemy.pet
objects/player.gd → objects/player.pet
8 × Write the type
2 slot(s) received both int and float, which a use tells apart.
objects/enemy.pet:30:13: hint: "amount" is left without a type, as no type fits all the values it receives: int (res://objects/enemy.pet:68), float (res://objects/player.pet:205).
5 × Make it pub
5 × Write .unwrap()
objects/enemy.pet:30:17: error: Parameter "amount" needs a type.
petrichor migrate: 7 script(s), 0 added, 0 left out, 8 type(s), 3 withdrawn, 16 fix(es), 9 path(s) renamed, 7 error(s), dry run
```

- `→` lines show where each file goes. A file pulled in by the paths you passed carries `added:` and the reason; a file that stays in GDScript has a `left out:` line instead.
- `N ×` lines count the types the tool wrote and each kind of fix it applied.
- `hint:` lines are worth reading. They mark types the tool withdrew or inferred from partial evidence, with the values that reached them, and code kept as GDScript runs it, such as calls by name.
- `error:` lines are the compile errors left in the migrated files, in the same format as `pet check`. The last line totals the report.

Here `damage(amount)` receives an `int` from the enemy and a `float` from the player, and subtracting it from an `int` health tells the two apart. The report counts such places on a line of their own. The tool does not write `float` for them, so the type of `amount` is left to you.

Errors left in the code do not fail the command: `migrate` exits with `1` only when it refuses or fails and writes nothing.

### What stays in GDScript

Some files stay in GDScript even when the whole project migrates. Each has a `left out:` line:

- scripts with `@tool` and their inheritance trees, until `@tool` arrives in 0.4;
- plugins under `addons/`, one line per plugin; a plugin migrates only when a path you pass lies inside it;
- script templates, from which the editor creates new scripts, in the folder that the `editor/script/templates_search_path` project setting names (`res://script_templates` by default);
- files that any of these uses at class level, for example by calling a static method or using an enum as a type, which GDScript cannot do with a Petrichor class.

Migrating part of a project also takes the files that have to move with it. These are the whole inheritance tree, since neither language can extend the other's classes, and the GDScript files that use it at class level. Migrating only `boss.gd` reports its base class as `enemy.gd → enemy.pet, added: in the inheritance tree of res://boss.gd`. The tool reads the remaining GDScript files but changes only their paths to renamed files.

### Finishing by hand

Wherever a change needs your judgment, the tool leaves a compile error. Open the project in Godot, or run `pet check`, to work through them. In Godot's script editor, click the light bulb beside an error line, press Alt+Enter, or choose **Quick Fix...** from the context menu to list its fixes. When a script has two or more errors with the same automatic fix, **Fix all '…' in file** applies them at once. VS Code offers the same fixes as code actions.

Review what the tool wrote around null:

- `.unwrap()` marks a value the original used without checking for null. It fails where GDScript would fail, so replace it with a check wherever null is expected.
- `T?` marks a value that can be null, such as an export that no scene sets, a member with no initial value, or a variable that receives `null`.
- `@late T` replaces `T?` on a member assigned after creation when every read already had `.unwrap()`, and those calls go away. Reading the member before it is assigned still fails at the same place.

The tool does not format the files; run `pet format` when you are done.

### Converting back

Converting back is for the Petrichor code written after migrating, for example when a team leaves after a trial. To undo a migration you just ran, restore the files with git instead. `--reverse` checks git in the same way, so commit the migration and your later work first.

The tool removes what GDScript has no syntax for, such as `pub`, `async`, `@late`, `.unwrap()` and the `?` of object types, and writes each type as far as GDScript can express it. A file stays a `.pet` file, together with its inheritance tree, when it has compile errors or uses a feature GDScript lacks, such as `struct`, payload enums, interfaces or `Result`. The report lists each place that keeps it, such as `error: GDScript has no struct.`

Converting back part of a project also converts the inheritance trees of the files you pass and the `.pet` trees they use at class level. The tool then checks the `.pet` files that stay against the converted classes as GDScript sees them, and lists the errors this causes. For example, a converted class's object members now read as `T?`. These errors do not stop the write. GDScript files that call the converted code are left to Godot, which reports any errors when it loads them.

Before writing, the tool parses each converted file back and compares it with the `.pet` file. If they differ, it writes nothing and asks you to report the output in [Issues](https://github.com/YYF233333/Petrichor/issues).

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
