# Hacking on cyanbox

Read `docs/OVERVIEW.md` first. This file is the house style and the rules
every module follows. The interfaces between modules are the headers in
`include/cyanbox/`; a module implements its header and uses the others only
through theirs.

## Language and style

- C23 (`-std=c23`), GCC. The only C++ is `src/ui/` (Dear ImGui),
  `src/player/stretch.cpp` (a C interface to Signalsmith Stretch's templates)
  and the vendored code. C++ files include C headers directly; every
  C header is already `extern "C"`-safe through plain C declarations, so wrap
  the includes in `extern "C" { }` on the C++ side.
- Every file starts with `/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */`
  and a comment saying what the file is for and the non-obvious decisions in
  it. Comments explain *why*; the code says what.
- Declare variables where they are first used. Never `for (;;)`: write
  `while (1)`.
- Errors are negative errno values (`CB_ERROR(EINVAL)`), 0 is success, counts
  are non-negative. Log with `cb_err/cb_warn/cb_info/cb_debug`; never print to
  stdout except from the CLI's own output.
- No global mutable state except the logger. A module that needs state owns
  a context struct.
- Allocation: `malloc`/`calloc`/`free` and the helpers in `cyanbox/common.h`.
  Check every allocation. The real-time audio thread never allocates.
- Every thread names itself first (`cb_thread_name`, `<module>:<what>` in
  the 15 bytes Linux keeps, which top -H, gdb and perf show): a pool's
  workers are `<name>:<i>`, a pipeline's thread `<name>:<codec>`.
- Names: `cb_<module>_<verb>` for functions, `CB<Thing>` for types,
  `CB_<THING>` for macros and enum values. Static helpers have no prefix.
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
  int cb_thing_open(CBThing **out, const char *path)
  {
      CBThing *t = calloc(1, sizeof(*t));
      if (!t)
          return CB_ERROR(ENOMEM);

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
  *out = (CBWaveDraw) {
      .x0 = r.x,  .y0 = r.y,
      .mode  = CB_WAVE_CENTRED,
      .alpha = 1.0f,
  };
  ```

- Comments are `/* ... */` in C, sentence case, wrapped at the same width,
  placed above the code they describe. `//` only in C++ and GLSL. No comment
  banners of `=` or `-` inside functions; section dividers in a file look like
  `/* ---------------------------------------------------------------- name */`.
- Names: `cb_module_verb()` functions, `CBThing` types (typedef'd structs and
  enums; the prefix is a contraction, so it is all caps, as `FFPacket` or
  `AVTConnection`), `CB_THING` macros and enum values, `static` helpers
  without prefix.
  Variables are short and lower case: `nb_tracks`, `out`, `ret`, `err`.
- `int ret = 0;` and `goto fail;` cleanup at the end of a function that
  allocates several things; return the first error.
- Error returns: `return CB_ERROR(EINVAL);`, checked as `if (ret < 0)`.
- Includes: the matching project header first (`"cyanbox/x.h"`), then system
  headers in a block, then library headers, then other project headers;
  blank lines between blocks.
- Every file starts with the one-line copyright comment and a paragraph
  saying what the file is for.

## Layout

```
include/cyanbox/    module interfaces (owned by the architecture, edit with care)
src/common        common.c log.c thread.c utf8.c nfc_tables.h         -> cb_common_dep
                  latin1.c kanji_readings.h (romaji: MeCab, Unihan, ICU)
src/audio         decode.c probe.c mp3.c flac.c artwork.c             -> cb_audio_dep (common)
                  encode.c source.c pipeline.c fifo.c input.c internal.h
src/core          model.c grid.c db.c library.c playlist.c config.c   -> cb_core_dep (common, audio)
                  notation.c wavestore.c core.h
src/analysis      spectrum.c onset.c tempo.c grid_fit.c downbeat.c    -> cb_analysis_dep (core, compute)
                  walk.c start.c beats.c beats_internal.h key.c
                  waveform.c analysis.c
src/formats       pdb.c anlz.c onelibrary.c sqlcipher.c sha512.h settings.c colors.c util.c (+ buf.h formats_priv.h nfc_tables.h) -> cb_formats_dep
                  colors.c util.c buf.h formats_priv.h
src/export        plan.c export.c inspect.c stick.c export_priv.h     -> cb_export_dep
src/link          packet.c presence.c (UDP presence) dbmsg.c catalog.c -> cb_link_dep (export, analysis)
                  dbserver.c dbclient.c (the database server) vtree.c
                  nfs.c nfsclient.c (NFS over a virtual tree) net.c
                  service.c (one interface's threads) set.c (every Ethernet
                  interface) client.c (the fake player) wav.c (the
                  WAVs made as they are read) wire.c link_priv.h (docs/LINK.md)
src/player        player.c pipewire.c nullout.c deck.c feed.c         -> cb_player_dep (core)
                  controls.c stretch.cpp stretch.h player_internal.h
src/app           jobs.c app.c                                        -> cb_app_dep (everything above,
                                                                       ml for the tag batch, link)
                  link.c applink.h (Link's switch, state and plays: --pro-link)
                  headless.c (src/gpu and src/ui stand-ins for cyanbox-cli and test_app)
                  mpris.c (MPRIS on the D-Bus session bus, with sd-bus: -Dmpris)
src/gpu           vkdev.c vkdev.h (the Vulkan set-up all three share) -> cb_vkdev_dep (common; Vulkan, VMA)
                  compute.c compute_dsp.c compute_priv.h              -> cb_compute_dep (vkdev)
                  shaders/fft.glsl stft.comp env.comp                 (no window: CLI, tests, tools)
                  window.c vk.c swapchain.c textures.c wave.c         (executable only)
                  imgui_glue.cpp gpu_priv.h gpu_demo.h shaders/
src/ml            gpu.c gguf.c tokenizer.c moss.c tag.c               -> cb_ml_dep (vkdev)
                  ml_priv.h kernels.h shaders/*.comp common.glsl      (MOSS-Audio, its own VkDevice;
                                                                       docs/ML.md, tests in suite ml)
src/ui            ui.cpp deck_view.cpp waveform_view.cpp browser.cpp  (C++)
                  tree.cpp dialogs.cpp keys.cpp link_view.cpp link_decks.cpp
                  finder.cpp ui_internal.h
src/cli.c         the subcommands; src/main.c
tests/<module>/   meson tests, registered from src/<module>/meson.build with suite: '<module>'
tools/            developer tools (analysis evaluation, dumpers; cyanbox-ml and ml_check.py
                  for the model's prompts and its encoder)
models/           the tagging model's GGUF file (not in git; .gitignore)
third_party/      imgui, vma (vendored, never edited); tomlc17 is kept for m3u/toml imports only
subprojects/      signalsmith-stretch, signalsmith-linear (git submodules, never edited)
docs/             OVERVIEW.md, this file, the format references from dj
```

Each `src/<module>/meson.build` builds its own static library, declares
`cb_<module>_dep`, and registers its tests with `test(..., suite: '<module>')`.
Only edit your own module's `meson.build`.

## Building

MASTER TEMPO's pitch shifter, Signalsmith Stretch, and the FFT library it
uses, signalsmith-linear, are git submodules in `subprojects/` (both MIT,
header-only, compiled into the player statically). After cloning, and
whenever a pull moves them:

```sh
git submodule update --init --recursive
```

The top-level `meson.build` reads them with meson's CMake module, so
`cmake` is needed at configure time. Stretch's CMakeLists.txt would fetch
signalsmith-linear over the network; `subprojects/cmake/` gives it a
stand-in to find instead, and the headers come from the submodule, so a
configure never downloads anything. To move either library, check out the
new commit in its submodule (signalsmith-linear at the tag Stretch's
CMakeLists.txt names) and commit the submodule.

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
- Audio tests use the null backend (`CB_AUDIO_NULL`). Never open the user's
  audio devices from a test.
- The analysis tests run the GPU front end too when there is a Vulkan
  device (no window or compositor needed); `CYANBOX_ANALYSIS_GPU=0` or `1`
  forces the CPU or the GPU path, `CYANBOX_GPU_DEVICE=NAME` picks the GPU,
  `CYANBOX_GPU_VALIDATION=1` adds the Khronos validation layer. A change to
  the front end's output bumps `CB_FRONTEND_VERSION` (beats_internal.h).

## Running the interface while developing

Never open windows on the user's session and never play audio through their
interface (the Focusrite). Use the private headless sway and a null sink:

```sh
# compositor (once; pick a socket name not in use)
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  sway -c tests/sway.conf -d > /tmp/sway-test.log 2>&1 &
grep -o "wayland display '[^']*'" /tmp/sway-test.log     # e.g. wayland-2

# a null audio sink to connect to
pw-cli create-node adapter '{ factory.name=support.null-audio-sink node.name=cyanbox-null media.class=Audio/Sink object.linger=true audio.position=[FL FR] }'

# the app, for 60 frames, then a screenshot
WAYLAND_DISPLAY=wayland-2 ./build/cyanbox --frames 60 --audio-sink cyanbox-null &
WAYLAND_DISPLAY=wayland-2 grim shot.png
```

`--audio none` uses the null backend with no PipeWire at all.

## The players' timeline

Every time in the model is on the players' timeline (cyanbox/audio.h): MP3s
include the leading header frame when rekordbox counts it and no encoder
delay is trimmed. The analyser, the deck and the stick writer all see the
same samples, so a cue set by ear in the interface lands on the same beat on
the CDJ.
