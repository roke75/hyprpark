# hyprpark

Park Hyprland windows in place without moving them to another workspace.

**This project’s code and documentation were generated entirely by AI under
human direction.**

hyprpark is a native compositor plugin. It hides a window while retaining its
workspace and floating state. Parked tiled windows release their space so
surrounding windows expand; restoring reinserts them into the current layout.
Floating windows keep their position and size. Maximized/fullscreen modes are
suspended while parked and reapplied on restore. It has no shelf or dock UI;
another shell or script can build one using its JSON list and change events.

## Requirements and build

Initially targets Hyprland **0.56.2** with Lua configuration. Build against the
headers matching the running compositor exactly. Other versions are untested.
The plugin refuses loading when the compositor/header API hashes differ.

On Arch Linux, the build dependencies are `hyprland`, `gcc`, `make`, `pkgconf`,
and `lua`. After cloning this repository:

```sh
make
hyprctl plugin load /absolute/path/to/hyprpark/build/hyprpark.so
```

Build before loading; do not overwrite a loaded shared library. Unload first
when rebuilding or updating:

```sh
hyprctl plugin unload /absolute/path/to/hyprpark/build/hyprpark.so
make clean
make
```

Unloading reveals and reinserts every mapped window owned by hyprpark. Rebuild
after Hyprland updates, and restart Hyprland when the running compositor and installed headers
no longer match. Loading is manual; no files or keybindings are installed.

## Lua API and keybindings

Load the plugin before using `hl.plugin.hyprpark`. `park(address)` and
`restore(address)` accept a window address such as `0x12345678`.
`restore_all()` reveals and reinserts all parked windows. Restoring does not
focus the window or switch workspaces; the shell can choose that behavior separately.

For example, after loading the plugin, add to `hyprland.lua`:

```lua
hl.bind("SUPER + M", hl.dsp.exec_cmd("/absolute/path/to/hyprpark/park-focused"))
hl.bind("SUPER + SHIFT + M", hl.dsp.exec_cmd(
    "hyprctl dispatch '(function() hl.plugin.hyprpark.restore_all(); return hl.dsp.no_op() end)()'"
))
```

The supplied `park-focused` helper requires `jq` and obtains the focused address
through IPC. The equivalent commands, including restore, are:

```sh
address=$(hyprctl activewindow -j | jq -r .address)
hyprctl dispatch "(function() hl.plugin.hyprpark.park('$address'); return hl.dsp.no_op() end)()"
hyprctl dispatch "(function() hl.plugin.hyprpark.restore('$address'); return hl.dsp.no_op() end)()"
hyprctl dispatch '(function() hl.plugin.hyprpark.restore_all(); return hl.dsp.no_op() end)()'
```

Lua-mode dispatch expressions must return a dispatcher. The wrappers above call
the plugin and return `hl.dsp.no_op()` after completing the operation.

This example requires `jq`; it is not a build dependency. Choose unused bindings.

## Shell integration

```sh
hyprctl hyprparkwindows
```

Returns a JSON array of owned, mapped window addresses, for example
`["0x12345678"]`. Closed windows are pruned when queried. Combine this with
`hyprctl clients -j` or your shell's toplevel model for titles and icons.

The compositor event socket emits `hyprpark>>` after park/restore/restore-all.
Listen for normal `closewindow` events too, since closed entries are pruned
lazily. Parked state belongs to the plugin and survives shell/config reloads.

Native dispatcher registrations are `hyprpark:park` (empty argument selects the
focused window), `hyprpark:restore` (address), and `hyprpark:restoreall`.
Address arguments also accept the `address:` prefix. For Lua-mode IPC, use the
Lua API expressions above rather than legacy dispatcher strings.

## Limitations

- Grouped and already-hidden windows cannot be parked.
- Restored tiled windows follow the current layout. Their previous tile position
  and split proportions are not preserved.
- Applications may suspend drawing while hidden, so captured previews can freeze.
- Floating geometry is retained; tiled geometry is recalculated by Hyprland.
- It uses internal Hyprland APIs and needs maintenance across compositor updates.
- Do not let multiple visibility plugins manage the same window.

## License

[MIT](LICENSE).
