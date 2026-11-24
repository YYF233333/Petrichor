# Getting Started

中文：[getting-started.zh.md](getting-started.zh.md)

This guide covers installing Petrichor, upgrading between builds, editing in VS Code, running the `pet` command-line tool, and building from source. For the language overview, design goals, and roadmap, see the [README](../README.md).

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
