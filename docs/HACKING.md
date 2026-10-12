# Hacking on rxbox

Read `docs/OVERVIEW.md` first. This file is the house style and the rules
every module follows. The interfaces between modules are the headers in
`include/rxbox/`; a module implements its header and uses the others only
through theirs.

## Language and style

- C23 (`-std=c23`), GCC. The only C++ is `src/ui/` (Dear ImGui) and the
  vendored code in `third_party/`. C++ files include C headers directly; every
  C header is already `extern "C"`-safe through plain C declarations, so wrap
  the includes in `extern "C" { }` on the C++ side.
- Every file starts with `/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */`
  and a comment saying what the file is for and the non-obvious decisions in
  it. Comments explain *why*; the code says what.
- Declare variables where they are first used. Never `for (;;)`: write
  `while (1)`.
- Errors are negative errno values (`RX_ERROR(EINVAL)`), 0 is success, counts
  are non-negative. Log with `rx_err/rx_warn/rx_info/rx_debug`; never print to
  stdout except from the CLI's own output.
- No global mutable state except the logger. A module that needs state owns
  a context struct.
- Allocation: `malloc`/`calloc`/`free` and the helpers in `rxbox/common.h`.
  Check every allocation. The real-time audio thread never allocates.
- Names: `rx_<module>_<verb>` for functions, `Rx<Thing>` for types,
  `RX_<THING>` for macros and enum values. Static helpers have no prefix.
- Warnings are errors for implicit declarations, missing prototypes, VLAs,
  pointer and int conversions; keep the build warning-free.
- No `for (;;)`, no `goto` except for cleanup labels, no VLAs.
- Every non-trivial rule about a file format or the players' behaviour cites
  its evidence in a comment (the dj docs, a rekordbox fixture, rbxport's
  measurement) the way dj's code does.

## Formatting (libcraw / avtransport style)

The style is the one of the user's libcraw and avtransport, exactly:

- 4-space indent, no tabs. Lines wrap around 100 columns.
- Function definitions: return type and name on one line, the opening brace
  on its own line. Control statements: `if (x) {` with the brace on the same
  line, `} else {` on one line, and no braces around a single statement:

  ```c
  int rx_thing_open(RxThing **out, const char *path)
  {
      RxThing *t = calloc(1, sizeof(*t));
      if (!t)
          return RX_ERROR(ENOMEM);

      for (int i = 0; i < t->nb; i++) {
          if (t->items[i].skip)
              continue;
          use(&t->items[i]);
      }

      *out = t;
      return 0;
  }
  ```

- `type *ptr`, not `type* ptr`. A space after `if`, `for`, `while`, `switch`,
  `return` (when followed by an expression), none after a function name or a
  cast: `(int)x`, `fn(a, b)`.
- Designated initialisers for structs, one field per line when more than
  two, aligned values where a block of assignments reads better that way:

  ```c
  *out = (RxWaveDraw) {
      .x0 = r.x,  .y0 = r.y,
      .mode  = RX_WAVE_CENTRED,
      .alpha = 1.0f,
  };
  ```

- Comments are `/* ... */` in C, sentence case, wrapped at the same width,
  placed above the code they describe. `//` only in C++ and GLSL. No comment
  banners of `=` or `-` inside functions; section dividers in a file look like
  `/* ---------------------------------------------------------------- name */`.
- Names: `rx_module_verb()` functions, `RXThing` types (typedef'd structs and
  enums; the prefix is a contraction, so it is all caps, as `FFPacket` or
  `AVTConnection`), `RX_THING` macros and enum values, `static` helpers
  without prefix. Note for the first implementation round: the headers were
  written as `RxThing`; keep using the names exactly as the headers spell
  them, and a single mechanical rename to `RXThing` is applied to the whole
  tree at integration.
  Variables are short and lower case: `nb_tracks`, `out`, `ret`, `err`.
- `int ret = 0;` and `goto fail;` cleanup at the end of a function that
  allocates several things; return the first error.
- Error returns: `return RX_ERROR(EINVAL);`, checked as `if (ret < 0)`.
- Includes: the matching project header first (`"rxbox/x.h"`), then system
  headers in a block, then library headers, then other project headers;
  blank lines between blocks.
- Every file starts with the one-line copyright comment and a paragraph
  saying what the file is for.

## Layout

```
include/rxbox/    module interfaces (owned by the architecture, edit with care)
src/common        common.c log.c thread.c                      -> rx_common_dep
src/audio         decode.c probe.c mp3.c flac.c artwork.c transcode.c   -> rx_audio_dep (common)
src/core          model.c grid.c db.c library.c playlist.c config.c notation.c -> rx_core_dep (common, audio)
src/analysis      spectrum.c onset.c tempo.c grid_fit.c downbeat.c beats.c key.c waveform.c analysis.c -> rx_analysis_dep
src/formats       pdb.c anlz.c onelibrary.c sqlcipher.c settings.c colors.c -> rx_formats_dep
src/export        plan.c export.c inspect.c sync.c              -> rx_export_dep
src/player        player.c pipewire.c nullout.c deck.c controls.c -> rx_player_dep (core)
src/app           jobs.c app.c                                  -> rx_app_dep (everything above)
src/gpu           window.c vk.c swapchain.c textures.c wave.c imgui_glue.cpp shaders/   (executable only)
src/ui            ui.cpp deck_view.cpp waveform_view.cpp browser.cpp tree.cpp dialogs.cpp keys.cpp (C++)
src/cli.c         the subcommands; src/main.c
tests/<module>/   meson tests, registered from src/<module>/meson.build with suite: '<module>'
tools/            developer tools (analysis evaluation, dumpers)
third_party/      imgui, vma (vendored, never edited); tomlc17 is kept for m3u/toml imports only
docs/             OVERVIEW.md, this file, the format references from dj
```

Each `src/<module>/meson.build` builds its own static library, declares
`rx_<module>_dep`, and registers its tests with `test(..., suite: '<module>')`.
Only edit your own module's `meson.build`.

## Working alongside other agents

Several agents implement modules at the same time in this tree. So:

- Touch only `src/<module>/`, `tests/<module>/` and `src/<module>/meson.build`.
  Never edit another module's files; if its header lacks something you
  need, work around it and report it.
- Use a private build directory: `meson setup build-<module>` once, then build
  your own targets by name (`ninja -C build-<module> src/<module>/librx_<module>.a
  <test-exe-names>`), and run `meson test -C build-<module> --no-rebuild
  --suite <module>`. Never run a bare `ninja` on the whole tree: another
  module may be half-written.
- If `meson setup` fails because another module's `meson.build` is broken at
  that moment, wait a minute and retry.
- Do not commit; do not run `git` write commands.
- `/tmp` is RAM: keep temporary files small and delete them.

## Tests

- `meson test -C build` runs everything. Tests are small C programs under
  `tests/<module>/` that return non-zero on failure and print what failed.
- Format writers are checked against the rekordbox fixtures in `tests/data`
  (from dj; see its README there), bit for bit where dj was bit for bit.
- The Python reference is next door: `~/projects/dj` (`.venv/bin/dj`,
  `.venv/bin/python -m pytest`). Ported code should reproduce its output; when
  a port deliberately differs (a fix from `docs/OVERVIEW.md` section 3), say so
  in a comment and in the test.
- Audio tests use the null backend (`RX_AUDIO_NULL`). Never open the user's
  audio devices from a test.

## Running the interface while developing

Never open windows on the user's session and never play audio through their
interface (the Focusrite). Use the private headless sway and a null sink:

```sh
# compositor (once; pick a socket name not in use)
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  sway -c tests/sway.conf -d > /tmp/sway-test.log 2>&1 &
grep -o "wayland display '[^']*'" /tmp/sway-test.log     # e.g. wayland-2

# a null audio sink to connect to
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=rxbox-null media.class=Audio/Sink object.linger=true audio.position=[FL FR] }'

# the app, for 60 frames, then a screenshot
WAYLAND_DISPLAY=wayland-2 ./build/rxbox --frames 60 --audio-sink rxbox-null &
WAYLAND_DISPLAY=wayland-2 grim shot.png
```

`--audio none` uses the null backend with no PipeWire at all.

## The players' timeline

Every time in the model is on the players' timeline (rxbox/audio.h): MP3s
include the leading header frame when rekordbox counts it and no encoder
delay is trimmed. The analyser, the deck and the stick writer all see the
same samples, so a cue set by ear in the interface lands on the same beat on
the CDJ.
