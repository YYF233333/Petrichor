# Petrichor

A statically typed scripting language for Godot, intended to replace GDScript and delivered as a GDExtension. Petrichor is the project codename.

**Status:** the design was restarted on 2026-10-23. The current decisions are recorded in the design decisions document; specification v0.1 is frozen as a reference (git tag `spec-v0.1`). There is no implementation yet.

- Design decisions: [English](docs/decisions.en.md) · [中文](docs/decisions.zh.md)
- Specification v0.2 draft, rewritten from the decisions: [English](docs/language-spec-draft.en.md) · [中文](docs/language-spec-draft.zh.md)
- Surveys:
  - [Do scripts see more than one scene structure?](docs/surveys/scene-structures.en.md) · [中文](docs/surveys/scene-structures.zh.md)
  - [How much code duplication do different native base classes force?](docs/surveys/cross-branch-duplication.en.md) · [中文](docs/surveys/cross-branch-duplication.zh.md)
  - [Pitfalls of the engine, GDScript and the C# bridge](docs/surveys/pitfalls.en.md) · [中文](docs/surveys/pitfalls.zh.md)
  - [Which numeric types do literal-initialized locals encounter?](docs/surveys/literal-local-constraints.en.md) · [中文](docs/surveys/literal-local-constraints.zh.md)
  - [How often is the result of awaiting a multi-parameter signal used?](docs/surveys/await-signal-results.en.md) · [中文](docs/surveys/await-signal-results.zh.md)
  - [How do real Godot 4 projects use property getters?](docs/surveys/property-accessors.en.md) · [中文](docs/surveys/property-accessors.zh.md)
  - [Where are script members used: own file, subclasses, other scripts?](docs/surveys/member-visibility.en.md) · [中文](docs/surveys/member-visibility.zh.md)
  - [What does it cost to migrate real code under the design decisions?](docs/surveys/paper-migration-2.en.md) · [中文](docs/surveys/paper-migration-2.zh.md)
  - [One-off counts in the corpus](docs/surveys/corpus-counts.en.md) · [中文](docs/surveys/corpus-counts.zh.md)
  - [What does it cost to migrate real code to spec v0.1?](docs/surveys/paper-migration.en.md) · [中文](docs/surveys/paper-migration.zh.md)
- Frozen v0.1 documents: specification ([English](docs/language-spec.en.md) · [中文](docs/language-spec.zh.md)), design notes ([English](docs/design-notes.en.md) · [中文](docs/design-notes.zh.md)), project plan ([English](docs/project-plan.en.md) · [中文](docs/project-plan.zh.md))

Feedback is welcome as GitHub issues.

---

一门面向 Godot 的静态类型脚本语言，目标是替代 GDScript，以 GDExtension 的形式交付。Petrichor 是项目代号。设计已于 2026-10-23 重开，现行决定见[设计决定](docs/decisions.zh.md)；v0.1 规范已冻结作为对照（git 标签 `spec-v0.1`）。目前还没有任何实现。欢迎通过 GitHub issue 反馈。

## License

[MIT](LICENSE)
