# Claude Instructions

## Project overview

GD Scene Tree is an OBS Studio plugin that adds a dock showing scenes as a tree
with folders, per-scene colours and icons, hide and lock, sorting, and a
settings dialog. Folder layouts are stored inside the OBS scene collection.

## Tech stack

- C++17 with Qt 6 Widgets and Qt Svg, against libobs and obs-frontend-api
- CMake, built through the OBS plugin template presets in `CMakePresets.json`
- GitHub Actions from the template for macOS, Windows and Ubuntu builds
- Lucide SVG icons bundled in `data/icons`

## Build commands

```sh
cmake --preset macos && cmake --build --preset macos
cmake --preset windows-x64 && cmake --build --preset windows-x64
cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64
```

macOS needs full Xcode for the template's Xcode generator. The configure step
downloads OBS sources and prebuilt dependencies listed in `buildspec.json`.

## Test commands

There are no automated tests. Verify changes by installing the built plugin
into OBS and exercising the dock. CI runs clang-format and gersemi on every
push to main, so run them before committing:

```sh
clang-format -i src/*.cpp src/*.hpp
gersemi -i CMakeLists.txt
```

## Architecture

- `src/plugin-main.cpp` registers and unregisters the dock.
- `src/scene-tree-dock.cpp` holds everything else: `SceneTreeWidget` paints
  rows, pills, hover and chevrons itself so the OBS theme cannot draw over them;
  `SceneTreeDock` owns the tree, toolbar, context menu, settings dialog, icon
  picker, persistence and the StreamUP import.
- Scenes are tracked by OBS UUID. Layouts are saved under `gd_scene_tree` in
  the scene collection through the frontend save callback. Global settings live
  in the plugin config file `settings.json`.
- `installer/windows.iss` is the Inno Setup script the Windows CI job runs on
  tagged releases.

## Conventions

- Keep the OBS code style enforced by `.clang-format`: tabs, 120 columns.
- Use OBS's own translated strings through `obs_frontend_get_locale_string`
  for entries that exist in OBS, and `data/locale/en-US.ini` for the rest.
- No comments in code. No emoji in commit messages or text.
- Commit messages: short, imperative mood, no attribution trailers.
