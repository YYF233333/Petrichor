# Trying Petrichor with this demo

This is Kenney's Starter Kit FPS with its eight scripts translated to Petrichor. `NOTICE.md` names the source and the licences. The GDScript originals are in [the kit's repository](https://github.com/KenneyNL/Starter-Kit-FPS), for comparing side by side.

## Run it

1. Open this folder with the official Godot 4.7 editor, standard or .NET.
2. Wait for the import to end. The first session also compiles the scripts.
3. Press F5.

On macOS, first run the `xattr` command from the [README](https://github.com/YYF233333/Petrichor#trying-it).

## Things to try

These are suggestions, and none is required. Each takes a minute and shows one check. Undo a change before trying the next.

1. **Rename a node that a script uses.** In `objects/player.tscn`, rename the node `Cooldown` to `ShotCooldown` and save. `player.pet` reads `$Cooldown`, so the editor reports:

   > res://objects/player.tscn: the root has no node at "Cooldown".

2. **Clear an export that must be set.** In `scenes/main.tscn`, select `Enemies/enemy-flying4` and clear `Player` in the Inspector. The enemy's script declares `@export pub var player: Node3D`, which is not nullable:

   > res://scenes/main.tscn: Enemies/enemy-flying4 has no value for "player", an export that must be assigned.

3. **Rename a method that a scene connects to.** In `objects/enemy.pet`, rename `_on_timer_timeout` to `_on_timeout`:

   > res://objects/enemy.tscn: the signal "timeout" of Timer is connected to _on_timer_timeout(), which this class does not declare.

4. **Change an interface.** In `scripts/combat.pet`, change `damage(amount: float)` to `damage(amount: int)`. `player.pet` and `enemy.pet` both implement it, and `player.pet` calls it with a `float`:

   > "damage()" does not have the signature that Combat.Damageable declares, func(int).
   >
   > Cannot pass a value of type float as int. Convert explicitly with int(x).

5. **Read a value that may be missing.** In `_process` of `objects/enemy.pet`, read the player through a nullable local:

   ```gdscript
   var target: Node3D? = player
   self.look_at(target.position + Vector3(0, 0.5, 0), Vector3.UP, true)
   ```

   > The value may be None (Node3D?); check it before reading "position".

The GDScript originals meet these mistakes only while the game runs. For the fourth, the original asks `has_method("damage")` at run time.

## Tell us what you find

Feedback on any aspect is welcome. That includes the language rules, the diagnostics, the editor, the installation, the documents and the direction of the project.

- Report defects as [issues](https://github.com/YYF233333/Petrichor/issues).
- Post everything else in [Discussions](https://github.com/YYF233333/Petrichor/discussions).

Both get a reply within 48 hours. The [release notes](https://github.com/YYF233333/Petrichor/releases) list where Petrichor differs from GDScript, and the questions we would most like answered.
