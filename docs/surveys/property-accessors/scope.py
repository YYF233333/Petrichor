"""Which part of the corpus a .gd file belongs to.

first-party  code whose upstream home is the repository itself: everything outside
             addons/, the addon(s) the repository exists to publish, and in
             godot-demo-projects the demo plugins under */addons/.
test         first-party unit tests (reported separately).
vendored     copies of addons whose upstream lives in another repository, even when
             the author is the same (e.g. Maaack's separate Scene Loader repository).
duplicate    generated copies of first-party files (Godot-Game-Template copies its
             addon's examples into scenes/ and scripts/ at the project root).
"""

# Addons that are the repository's own product. Every other addons/<name> is vendored,
# except in godot-demo-projects, whose addons are part of the demos.
OWN_ADDONS = {
    'beehave': {'beehave'},                    # vendored: gdUnit4
    'gdUnit4': {'gdUnit4'},
    'godot_dialogue_manager': {'dialogue_manager'},
    'Godot-Game-Template': {'maaacks_game_template'},  # vendored: music/scene loader/ui sound
                                                      # controllers, plugin_updater, clean_copy_examples
    'godot-open-rpg': set(),                   # vendored: dialogic
    'godot-statecharts': {'godot_state_charts'},  # vendored: gut
    'phantom-camera': {'phantom_camera'},
    'Pixelorama': set(),                       # vendored: aimg_io, applinks, dockable_container,
                                               # gdgifexporter, keychain, SmartSlicer
}

TEST_DIRS = {
    'beehave': ('test/',),
    'gdUnit4': ('addons/gdUnit4/test/',),
    'godot_dialogue_manager': ('tests/',),
    'godot-statecharts': ('tests/',),
}

DUPLICATE_DIRS = {
    'Godot-Game-Template': ('scenes/', 'scripts/'),
}


def scope_of(repo, rel):
    """rel is the path relative to the repository root, with forward slashes."""
    for d in DUPLICATE_DIRS.get(repo, ()):
        if rel.startswith(d):
            return 'duplicate'
    for d in TEST_DIRS.get(repo, ()):
        if rel.startswith(d):
            return 'test'
    parts = rel.split('/')
    if 'addons' in parts and repo != 'godot-demo-projects':
        name = parts[parts.index('addons') + 1]
        if name not in OWN_ADDONS.get(repo, set()):
            return 'vendored'
    return 'first-party'
