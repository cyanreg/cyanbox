# "My Settings" files: `MYSETTING.DAT`, `MYSETTING2.DAT`, `DJMMYSETTING.DAT`, `DEVSETTING.DAT`

Writer reference. All multi-byte integers in these files are **little-endian**,
unlike the ANLZ files. Offsets are given both as **file offsets** and as
**payload offsets** (payload = data section, starting at file offset 104 = 0x68).

**UNCERTAIN** marks anything that is not settled. Where sources disagree,
both versions are given.

---

## 0. Sources and licences

| Tag | Source | Licence | Used for |
|---|---|---|---|
| [RC] | rekordcrate `src/setting.rs` and test data `data/{mysetting,mysetting2,djmmysetting,devsetting}/*`, `data/complete_export/*/PIONEER/*.DAT`. These are real rekordbox 6.6.1 files, one per changed setting. <https://github.com/Holzhaus/rekordcrate> (14d54ed) | MPL-2.0 | Layout, enums, defaults, **ground-truth files** |
| [PRB] | pyrekordbox `pyrekordbox/mysettings/{structs,file}.py`, `docs/source/formats/mysetting.md`, `.testdata/**/*.DAT` (rekordbox 5.8.6, 6.6.2). <https://github.com/dylanljones/pyrekordbox> (5feacbc) | MIT | Layout, CRC, defaults, older-version files |
| [RBOX] | rbox 0.1.7 `src/settings.rs` (<https://docs.rs/rbox>) | GPL-3.0-only | Cross-check (identical to [RC]) |
| [BK] | baken `crates/baken-export/src/settings.rs`, README "expressport". <https://github.com/M-Igashi/baken> (194ef79) | MIT | CRC coverage check, file sizes, where files go, hardware tests |
| [SB] | Seabass `src/infrastructure/rekordbox/rekordbox_settings_fields.*`, fixture `tests/fixtures/anonymized_library/rekordbox/*.DAT` (a **rekordbox 7.2.8** export). <https://github.com/sebasje/seabass> (397bb6e) | code GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL; fixtures CC0-1.0 | Field table, rekordbox 7 files |
| [MAN] | Pioneer DJ CDJ-3000 Instruction Manual (Utility settings, Shortcut screen, My Settings save/load), copy at <https://cdn01.4wall.com/cms/rentals/files/f6835e4eabc543.pdf> | © AlphaTheta, facts only | Player-side behaviour, factory defaults |
| [ME] | My own parse of all the files above: CRC verification, diff of every rekordcrate fixture against the default file | n/a | Every enum value below was confirmed by a real file unless marked otherwise |

---

## 1. Files, location, who writes them

* Location on the stick: **`/PIONEER/MYSETTING.DAT`, `/PIONEER/MYSETTING2.DAT`,
  `/PIONEER/DJMMYSETTING.DAT`, `/PIONEER/DEVSETTING.DAT`** (inside `PIONEER/`, not the root)
  [RC] [BK].
* Local copies of the rekordbox files: `~/Library/Application Support/Pioneer/rekordbox6/` (macOS) or
  `%APPDATA%\Pioneer\rekordbox6\` (Windows) [BK] [RC]. rekordbox writes them
  once Preferences > DJ System > My Settings has been opened, which is only possible in EXPORT mode [BK].
* `MYSETTING.DAT` / `MYSETTING2.DAT`: CDJ/XDJ player settings. `DJMMYSETTING.DAT`: mixer
  settings. `DEVSETTING.DAT`: per-storage-device display settings (rekordbox Preferences > General;
  on the CDJ-3000 these are the [SHORTCUT] > "Device settings" items: waveform colour and waveform
  current position [MAN]).
* **Players also write these files.** The CDJ-3000 can SAVE My Settings to a stick [MAN], and
  [BK] found a `DEVSETTING.DAT` written by a player on a real stick. A rekordbox 7 export does
  not always include `DEVSETTING.DAT`, and a CDJ-2000NXS2 reads a stick without it [BK].
* **How players apply them** [MAN] (CDJ-3000): My Settings are **not** applied automatically. The DJ
  loads them via [SOURCE] > device > [SHORTCUT] > MY SETTINGS [LOAD], or via the device-info
  "[MY SETTINGS LOAD]" item. Loading is refused while a track is playing. A player LOADs them
  over PRO DJ LINK from another player's stick or from rekordbox (or cyanbox) as well
  (docs/LINK.md, "My Settings over the link").
  The settings covered are the Utility items marked as duplicable, plus time display mode, Auto Cue,
  jog mode, tempo range, Master Tempo, Quantize, Beat Sync and phase meter.
* No CDJ-3000-specific settings file is known beyond these four. Newer rekordbox versions have
  **extended the payloads in place** (bytes that used to be zero are now set; §7).
  Players also leave other files on sticks: `PIONEER/CDJ…` [BK], and `RBFLTR.DAT` and `export.pdb.bak`
  (reported by fourfour, CDJ-3000 fw 3.19; see anlz.md §0 [FF]). These are not settings files,
  and writers should leave them alone.

---

## 2. Container layout (all four files)

Sources: [RC] `Setting`, [PRB] structs/docs, [BK] `validate()`, confirmed [ME].

| File off | Size | Field | Value |
|---|---|---|---|
| 0x00 | 4 | len_strings (u32 LE) | `0x00000060` (96 = 3 × 32) |
| 0x04 | 32 | brand (ASCII, NUL-padded) | `"PIONEER"` (MYSETTING, MYSETTING2), `"PioneerDJ"` (DJMMYSETTING), `"PIONEER DJ"` (DEVSETTING) |
| 0x24 | 32 | software | `"rekordbox"` |
| 0x44 | 32 | version | `"0.001"` (MYSETTING, MYSETTING2), `"1.000"` (DJMMYSETTING), **the rekordbox version** for DEVSETTING (`"5.8.6"`, `"6.6.1"`, `"6.6.2"`, `"7.2.8"` seen) |
| 0x64 | 4 | len_data (u32 LE) | 40 (MYSETTING, MYSETTING2), 52 (DJMMYSETTING), 32 (DEVSETTING) |
| 0x68 | len_data | payload | see below |
| 0x68+len | 2 | checksum, u16 **LE** | CRC-16/XMODEM, see §2.1 |
| 0x6A+len | 2 | `0x0000` | |

File sizes: MYSETTING 148, MYSETTING2 148, DJMMYSETTING 160, DEVSETTING 140 bytes [BK] [ME].

### 2.1 Checksum

CRC-16/XMODEM: poly 0x1021, init 0x0000, no reflection, no final XOR; check value of
`"123456789"` is 0x31C3. It is stored **little-endian**.

**Which bytes are covered differs by file** ([RC], [PRB], [BK]; verified on every real file [ME]):

| File | CRC covers |
|---|---|
| MYSETTING.DAT, MYSETTING2.DAT, DEVSETTING.DAT | payload only: file bytes `[0x68, 0x68+len_data)` |
| DJMMYSETTING.DAT | **everything before the CRC**: file bytes `[0x00, 0x68+len_data)` (header included) |

```c
uint16_t crc16_xmodem(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
```

Known-good values (rekordbox 6.6.1 defaults, [RC] `complete_export/empty`): MYSETTING `0x7A48`
(bytes `48 7A`), MYSETTING2 `0xBB40`, DJMMYSETTING `0x6C38`, DEVSETTING `0xD016`.

What a player does with a bad CRC is **UNCERTAIN** (nobody has reported a test). [BK] refuses to export files
whose CRC does not verify.

### 2.2 Encoding convention

Almost every setting is one byte holding an enum based at **0x80** ("0x80 = first option"). A few
use base 0x81 (LCD brightness, language, pad/jog-LCD brightness) [PRB]. DEVSETTING uses base
**0x01**. Payloads start with the magic `78 56 34 12` (0x12345678 LE) and a small version word.

---

## 3. `MYSETTING.DAT` payload (40 bytes): player settings

Sources: [RC] `MySetting` + fixtures, [PRB] `MySettingBody`, [SB] field table. Defaults = what rekordbox
6.6.1 writes [RC] (same in [PRB] defaults and in the rekordbox 6.6.2 test file). "✓" = value
confirmed by a real rekordbox file that changes only that setting [ME].

| Payload off | File off | Setting (rekordbox / CDJ name) | Values | rekordbox default |
|---|---|---|---|---|
| 0–7 | 0x68 | magic/version | `78 56 34 12 02 00 00 00` | – |
| 8 | 0x70 | ON AIR DISPLAY | 0x80 off ✓, 0x81 on | 0x81 on |
| 9 | 0x71 | LCD BRIGHTNESS | 0x81 1 ✓ … 0x85 5 ✓ | 0x83 (3) |
| 10 | 0x72 | QUANTIZE | 0x80 off ✓, 0x81 on | 0x81 on |
| 11 | 0x73 | AUTO CUE LEVEL | 0x80 −36 dB ✓, 0x81 −42 ✓, 0x82 −48, 0x83 −54, 0x84 −60, 0x85 −66, 0x86 −72 ✓, 0x87 −78 dB ✓, 0x88 MEMORY | 0x88 MEMORY |
| 12 | 0x74 | LANGUAGE | 0x81 English, 0x82 French ✓, 0x83 German, 0x84 Italian, 0x85 Dutch, 0x86 Spanish, 0x87 Russian, 0x88 Korean, 0x89 Chinese (simpl.), 0x8A Chinese (trad.), 0x8B Japanese, 0x8C Portuguese, 0x8D Swedish, 0x8E Czech, 0x8F Hungarian, 0x90 Danish, 0x91 Greek ✓, 0x92 Turkish ✓ | 0x81 English |
| 13 | 0x75 | unknown | `0x01` | 0x01 |
| 14 | 0x76 | JOG RING BRIGHTNESS | 0x80 off ✓, 0x81 dark (1) ✓, 0x82 bright (2) | 0x82 |
| 15 | 0x77 | JOG RING INDICATOR | 0x80 off ✓, 0x81 on | 0x81 |
| 16 | 0x78 | SLIP FLASHING | 0x80 off ✓, 0x81 on | 0x81 |
| 17–19 | 0x79 | unknown | `01 01 01` | `01 01 01` |
| 20 | 0x7C | DISC SLOT ILLUMINATION | 0x80 off ✓, 0x81 dark ✓, 0x82 bright | 0x82 |
| 21 | 0x7D | EJECT/LOAD LOCK | 0x80 unlock, 0x81 lock | 0x80 unlock (cyanbox: 0x81 lock) |
| 22 | 0x7E | SYNC (Beat Sync initial state) | 0x80 off, 0x81 on ✓ | 0x80 off |
| 23 | 0x7F | PLAY MODE / AUTO PLAY MODE | 0x80 continue, 0x81 single | 0x81 single |
| 24 | 0x80 | QUANTIZE BEAT VALUE | 0x80 1 beat, 0x81 1/2 ✓, 0x82 1/4 ✓, 0x83 1/8 ✓ | 0x80 (1) |
| 25 | 0x81 | HOT CUE AUTO LOAD | 0x80 off ✓, 0x81 on, 0x82 rekordbox SETTING ✓ | 0x81 on |
| 26 | 0x82 | HOT CUE COLOR (pads lit in cue colours) | 0x80 off, 0x81 on ✓ | 0x80 off |
| 27–28 | 0x83 | unknown | `00 00` | `00 00` |
| 29 | 0x85 | NEEDLE LOCK | 0x80 unlock, 0x81 lock | 0x81 lock |
| 30–31 | 0x86 | unknown | `00 00` | `00 00` |
| 32 | 0x88 | TIME MODE | 0x80 elapsed ✓, 0x81 remain | 0x81 remain |
| 33 | 0x89 | JOG MODE | 0x80 CDJ ✓, 0x81 VINYL | 0x81 vinyl |
| 34 | 0x8A | AUTO CUE | 0x80 off ✓, 0x81 on | 0x81 on |
| 35 | 0x8B | MASTER TEMPO | 0x80 off, 0x81 on ✓ | 0x80 off |
| 36 | 0x8C | TEMPO RANGE | 0x80 ±6 % ✓, 0x81 ±10 %, 0x82 ±16 % ✓, 0x83 WIDE ✓ | 0x81 (±10 %) |
| 37 | 0x8D | PHASE METER | 0x80 type 1, 0x81 type 2 ✓ | 0x80 |
| 38–39 | 0x8E | unknown | `00 00` | `00 00` |

* [PRB]'s doc lists hot-cue-autoload "rekordbox" as `0x83`. Its code and the real file use **0x82**.
* **cyanbox's default TEMPO RANGE is ±6 % (`0x80`)**, not rekordbox's ±10 %: a library with no
  `player.tempo_range` exports `0x80`, and cyanbox's own deck starts at ±6 % as a CDJ switched on
  does. **cyanbox's default WAVEFORM COLOR is 3-band (`0x04`)**, not rekordbox's blue, for the
  same reason: the interface draws the palette this setting holds (the 3-band one in cyanbox's
  own colours, docs/UI.md). **cyanbox's default EJECT/LOAD LOCK is lock (`0x81`)**, not
  rekordbox's unlock: a deck that plays keeps its track, on the player and from the interface's
  "Load in deck" (docs/UI.md, which greys a playing deck out while the lock is on). Every other
  default is rekordbox's. cyanbox's interface follows
  these settings (docs/UI.md, Player settings), so the range the deck starts at and the palette
  it draws are the ones the stick carries.
* CDJ-3000 factory defaults [MAN] mostly equal these. The CDJ-3000 jog-ring brightness is labelled "OFF, 1, 2".
* Real rekordbox 7.2.8 MYSETTING payload [SB] ([ME]): same 40-byte layout, no new fields
  (`78563412 02000000 81 83 81 88 81 01 82 81 81 010101 82 80 80 81 80 81 80 0000 81 0000 81 81 81 80 80 80 0000`).

Full default file (rekordbox 6.6.1, 148 bytes; header strings NUL-padded):
```
0x00: 60 00 00 00  "PIONEER" + 25×00
0x24: "rekordbox" + 23×00
0x44: "0.001" + 27×00
0x64: 28 00 00 00
0x68: 78 56 34 12 02 00 00 00 81 83 81 88 81 01 82 81
0x78: 81 01 01 01 82 80 80 81 80 81 80 00 00 81 00 00
0x88: 81 81 81 80 81 80 00 00
0x90: 48 7A 00 00
```

---

## 4. `MYSETTING2.DAT` payload (40 bytes): more player settings (NXS2 / CDJ-3000 era)

Sources: [RC] `MySetting2` + fixtures, [PRB] `MySetting2Body`, [SB].

| Payload off | File off | Setting | Values | rekordbox default |
|---|---|---|---|---|
| 0 | 0x68 | VINYL SPEED ADJUST | 0x80 touch & release ✓, 0x81 touch, 0x82 release ✓ | 0x81 touch |
| 1 | 0x69 | JOG DISPLAY MODE | 0x80 auto, 0x81 info ✓, 0x82 simple ✓, 0x83 artwork ✓ | 0x80 auto |
| 2 | 0x6A | PAD BUTTON BRIGHTNESS | 0x81 1 ✓, 0x82 2, 0x83 3, 0x84 4 ✓ | 0x83 |
| 3 | 0x6B | JOG LCD BRIGHTNESS | 0x81 1 ✓ … 0x85 5 ✓ | 0x83 |
| 4 | 0x6C | WAVEFORM DIVISIONS | 0x80 time scale ✓, 0x81 phrase | 0x81 phrase |
| 5–9 | 0x6D | unknown: zeros in rekordbox 5/6; **rekordbox 7.2.8 writes `80 80 00 00 00`** (§7) | – | `00 00 00 00 00` |
| 10 | 0x72 | WAVEFORM / PHASE METER (what the main waveform area shows) | 0x80 waveform, 0x81 phase meter ✓ | 0x80 |
| 11 | 0x73 | unknown | `0x81` | 0x81 |
| 12 | 0x74 | BEAT JUMP BEAT VALUE | 0x80 1/2 ✓, 0x81 1 ✓, 0x82 2, 0x83 4, 0x84 8, 0x85 16, 0x86 32, 0x87 64 ✓ | 0x85 (16) |
| 13–39 | 0x75 | unknown | 27 × 00 | zeros |

Default file payload (rekordbox 6.6.1): `81 80 83 83 81 00 00 00 00 00 80 81 85` + 27×00, CRC `40 BB`.

---

## 5. `DJMMYSETTING.DAT` payload (52 bytes): mixer settings

Sources: [RC] `DJMMySetting` + fixtures, [PRB] `DjmMySettingBody`.

| Payload off | File off | Setting | Values | rekordbox default |
|---|---|---|---|---|
| 0–11 | 0x68 | magic/version | `78 56 34 12 01 00 00 00 20 00 00 00` | – |
| 12 | 0x74 | CH FADER CURVE | 0x80 steep top ✓, 0x81 linear, 0x82 steep bottom ✓ | 0x81 |
| 13 | 0x75 | CROSSFADER CURVE | 0x80 constant power ✓, 0x81 slow cut ✓, 0x82 fast cut | 0x82 |
| 14 | 0x76 | HEADPHONES PRE EQ | 0x80 post EQ, 0x81 pre EQ ✓ | 0x80 |
| 15 | 0x77 | HEADPHONES MONO SPLIT | 0x80 stereo, 0x81 mono split ✓ | 0x80 |
| 16 | 0x78 | BEAT FX QUANTIZE | 0x80 off ✓, 0x81 on | 0x81 |
| 17 | 0x79 | MIC LOW CUT | 0x80 off ✓, 0x81 on | 0x81 |
| 18 | 0x7A | TALK OVER MODE | 0x80 advanced, 0x81 normal ✓ | 0x80 |
| 19 | 0x7B | TALK OVER LEVEL | 0x80 −24 dB ✓, 0x81 −18 dB, 0x82 −12 dB ✓, 0x83 −6 dB ✓ | 0x81 |
| 20 | 0x7C | MIDI CH | 0x80 ch 1 … 0x8F ch 16 (✓ 2, 15, 16) | 0x80 |
| 21 | 0x7D | MIDI BUTTON TYPE | 0x80 toggle, 0x81 trigger ✓ | 0x80 |
| 22 | 0x7E | BRIGHTNESS > DISPLAY | 0x80 WHITE ✓, 0x81 1 ✓, 0x82 2 ✓, 0x83 3, 0x84 4, 0x85 5 | 0x85 |
| 23 | 0x7F | BRIGHTNESS > INDICATOR | 0x80 1 ✓, 0x81 2 ✓, 0x82 3 | 0x82 |
| 24 | 0x80 | CH FADER CURVE (LONG FADER) | 0x80 exponential, 0x81 smooth ✓ ("exp2" in [RC] fixture name), 0x82 linear ✓ | 0x80 |
| 25–51 | 0x81 | unknown: 27 × 00 in rekordbox 6; **rekordbox 7.2.8 writes `80 82 84 81 81` at 25–29** (§7) | – | zeros |

Remember: this file's CRC covers the header as well (§2.1). Default CRC `38 6C`.

---

## 6. `DEVSETTING.DAT` payload (32 bytes): per-device display settings

Sources: [RC] `DevSetting` + fixtures, [PRB] (header only, which it says it does not decode), [ME] across
rekordbox 5.8.6 / 6.6.1 / 6.6.2 / 7.2.8 files, [MAN] (CDJ-3000 "Device settings").

| Payload off | File off | Setting | Values (base **0x01**) | default |
|---|---|---|---|---|
| 0–8 | 0x68 | magic/version | `78 56 34 12 01 00 00 00 01` | – |
| 9 | 0x71 | Type of the overview waveform | 0x01 half waveform, 0x02 full waveform ✓ | 0x01 |
| 10 | 0x72 | WAVEFORM COLOR | 0x01 BLUE, 0x03 RGB ✓, 0x04 3BAND ✓ (**no 0x02**) | 0x01 (cyanbox: 0x04) |
| 11 | 0x73 | unknown | `0x01` | 0x01 |
| 12 | 0x74 | Key display format | 0x01 classic, 0x02 alphanumeric ✓ | 0x01 |
| 13 | 0x75 | WAVEFORM CURRENT POSITION | 0x01 center, 0x02 left ✓ | 0x01 |
| 14–16 | 0x76 | unknown: **0 in rekordbox ≤ 6.6.2, `01 01 01` in rekordbox 7.2.8** (§7) | – | – |
| 17–31 | 0x79 | zeros | – | – |

* rekordbox 5.8.6 wrote offset 13 as `0x00`: the waveform-position setting did not exist yet.
* Version string = rekordbox version.
* [PRB] writes `78 56 34 12 02 00 00 00 …` as the DEVSETTING magic. Real files have `01 00 00 00 01`
  ([RC] asserts this). Use the real value.
* [RC] **asserts** offsets 14–31 are zero, so it would reject a rekordbox 7.2.8 file.
* Waveform colour selects which ANLZ tags the player draws: BLUE (PWAV/PWV3), RGB (PWV4/PWV5),
  3BAND (PWV6/PWV7, CDJ-3000). See anlz.md.

Default file (rekordbox 6.6.1): payload `78 56 34 12 01 00 00 00 01 01 01 01 01 01` + 18×00, version `"6.6.1"`, CRC `16 D0`.

---

## 7. rekordbox 7 additions (unknown fields)

Compared with rekordbox 6.6.x, the rekordbox 7.2.8 files [SB] set previously-zero bytes [ME]:

| File | Payload off | 6.6.x | 7.2.8 |
|---|---|---|---|
| MYSETTING2 | 5, 6 | `00 00` | `80 80` |
| DJMMYSETTING | 25–29 | `00 00 00 00 00` | `80 82 84 81 81` |
| DEVSETTING | 14–16 | `00 00 00` | `01 01 01` |
| MYSETTING | – | – | unchanged |

Their meanings are **UNKNOWN**. They are presumably settings for newer hardware (CDJ-3000X, DJM-A9/V10, etc.) given the
0x80/0x01 enum bases. Players accept the 6.6.x layout with zeros there ([BK] tested copied files on CDJ-3000 3.22 and
CDJ-2000NXS2. Whether those were 6.x or 7.x files is not recorded). For maximum safety, write the
6.6.1 layout, or copy these exact 7.2.8 values if you want to mimic rekordbox 7.

---

## 8. Writer checklist

1. Build the 104-byte header: `60 00 00 00`, three 32-byte NUL-padded ASCII strings, `len_data` LE.
2. Fill the payload from the tables; keep the unknown constant bytes (`01`, `01 01 01`, `81`, magic words).
3. CRC-16/XMODEM over the payload (MYSETTING, MYSETTING2, DEVSETTING) or over header + payload
   (DJMMYSETTING). Store the CRC little-endian, then `00 00`.
4. Put the files in `/PIONEER/`. Writing none is a valid choice: players keep their own settings, and
   the DJ must explicitly LOAD them anyway [MAN] [BK].
5. Never emit an enum value outside the listed sets. Behaviour on unknown values is untested.
