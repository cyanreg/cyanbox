# OneLibrary (`exportLibrary.db`, a.k.a. "Device Library Plus"): writer reference

OneLibrary is an encrypted SQLite database (SQLCipher) that rekordbox ≥ 6.8 / 7.x writes
to `PIONEER/rekordbox/` on USB/SD media, alongside the legacy DeviceSQL `export.pdb`
("Device Library"). AlphaTheta, Algoriddim (djay Pro) and Native Instruments (Traktor) adopted it
as a cross-vendor format in October 2025.

**UNCERTAIN** marks anything that is not settled. Conflicting claims are listed side by side.

---

## 0. Sources and licences

| Tag | Source | Licence | Used for |
|---|---|---|---|
| [PRB] | pyrekordbox `pyrekordbox/devicelib_plus/{database,models}.py`, `pyrekordbox/utils.py` (`deobfuscate`), `docs/source/formats/devicelib_plus.md`, <https://github.com/dylanljones/pyrekordbox> (5feacbc) | MIT | Key blob, deobfuscation, ORM model, column docs |
| [OLC] | onelibrary-connect `src/encryption.ts`, `src/connection.ts`, `src/schema.ts`, <https://github.com/chrisle/onelibrary-connect> (ac3edd3) | MIT | Independent key derivation, `cipher=sqlcipher; legacy=4` |
| [RBOX] | rbox 0.1.7 `src/one_library/*`, `migrations/2025-11-20-141825_initial/up.sql` (crates.io; <https://docs.rs/rbox>) | GPL-3.0-only | A writer's schema (deviates from real files, see §3.4) |
| [SB] | Seabass `docs/onelibrary-format.md`, `src/infrastructure/onelibrary/*`, fixtures `tests/fixtures/{onelibrary_playlist_tree/0-base,anonymized_library,one_stick_two_catalogs}`, <https://github.com/sebasje/seabass> (397bb6e) | code GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL; docs CC-BY-SA-4.0; **fixtures CC0-1.0** | Hardware observations (CDJ-3000X, OMNIS-DUO); **a pristine rekordbox 7.2.18 `exportLibrary.db`** |
| [FF] | fourfour `pioneer-usb-writer/reference-code/PIONEER.md`, `src/writer/onelibrary.rs`, `AGENTS.md`, <https://github.com/morizkraemer/fourfour> (64091d3) | **no licence** (all rights reserved). Facts only | Stick layout, from-scratch writer notes |
| [BK] | baken README / RELEASE_NOTES, <https://github.com/M-Igashi/baken> (194ef79) | MIT | What rekordbox puts on the stick; consistency gotchas |
| [BL] | beat-link `OpusProvider.java` (`SQLiteMCSqlCipherConfig.getV4Defaults()`), crate-digger `Archivist.java`, <https://github.com/Deep-Symmetry/beat-link> | EPL-2.0 | SQLCipher v4 defaults confirmation |
| [GIST] | 0xdevalias, "Reverse engineering Pioneer/AlphaTheta rekordbox …" gist, <https://gist.github.com/0xdevalias/b803476793b56f7c45e6361799168eb0> | unspecified | Project list, key cross-check |
| [AT-G] | AlphaTheta, "OneLibrary Compatible USB Device Export Guide" (rekordbox ≥ 7.2.11), <https://cdn.rekordbox.com/files/20260317132052/OneLibrary-Compatible-USB-Device-Export-.pdf> | © AlphaTheta, facts only | Official device split, file systems |
| [AT-A] | rekordbox.com announcement "DJ brands unite to launch OneLibrary" (Oct 2025), <https://rekordbox.com/en/2025/10/dj-brands-unite-to-launch-onelibrary/> | © AlphaTheta, facts only | Supported software/hardware |
| [MAN] | CDJ-3000 Instruction Manual (device BACKGROUND COLOR, My Settings) | © AlphaTheta | `backGroundColorType` context |
| [ME] | My own work: I wrote a minimal SQLCipher-4 decryptor (stdlib `hashlib` + `cryptography` AES), verified every page HMAC, applied the `-wal`, and dumped schema and rows from the [SB] fixtures above with Python's `sqlite3` | n/a | Parameters, real schema, real values |

---

## 1. Encryption

### 1.1 Parameters: plain SQLCipher 4 defaults

Confirmed by decrypting three real files with **every page HMAC verifying** [ME] (31-, 35- and
144-page DBs, plus the WAL frames of one of them). The parameters agree with [SB] (opened with `PRAGMA key` only),
[BL] (`getV4Defaults()`), [OLC] (`legacy = 4`) and [FF].

| Parameter | Value |
|---|---|
| SQLCipher compatibility | 4 (`PRAGMA cipher_compatibility = 4`, the default in SQLCipher 4.x; SQLite3MultipleCiphers: `cipher=sqlcipher`, `legacy=4`) |
| Page size | 4096 |
| Cipher | AES-256-CBC, fresh random 16-byte IV per page write |
| KDF | PBKDF2-HMAC-SHA512, **256 000** iterations, 32-byte key; salt = file bytes 0..15 |
| HMAC | HMAC-SHA512 over `ciphertext ‖ IV ‖ page_no (u32 LE, 1-based)`; HMAC key = PBKDF2-HMAC-SHA512(enc_key, salt XOR 0x3A, **2** iterations, 32 bytes) |
| Reserve per page | 80 bytes = 16 IV + 64 HMAC (plaintext header byte 20 = 80) |
| Page 1 | bytes 0..15 = salt (in place of `"SQLite format 3\0"`); encrypted region 16..4015 |
| Passphrase use | `PRAGMA key = '<passphrase>'` (a **text passphrase** run through the KDF, **not** a raw `x'…'` key) |
| Journal | **WAL** (header bytes 18/19 = 2). rekordbox leaves `exportLibrary.db-wal` and `-shm` beside it. WAL frames are encrypted the same way (verified [ME]) |
| Text encoding / user_version / application_id / auto_vacuum | UTF-8 / 0 / 0 / none |
| SQLite version that last wrote the file (header @96) | 3045003 (**3.45.3**) in the pristine rekordbox **7.2.18** file; 3.44.2 in two files later modified by Seabass. **UNCERTAIN**: rekordbox's exact SQLCipher version (4.6.x carries 3.45.3) |

### 1.2 Passphrase

**Public and identical for all users and devices** ([SB], [PRB], [OLC], [FF], [GIST]; re-derived independently [ME]):

```
r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls
```

(64 ASCII characters.) rekordbox stores it obfuscated. pyrekordbox ships the blob
(`devicelib_plus/database.py`), and the derivation is:

```
BLOB = "PN_1dH8$oLJY)16j_RvM6qphWw`476>;C1cWmI#se(PG`j}~xAjlufj?`#0i{;=glh(SkW)y0>n?YEiD`l%t("
1. base85-decode BLOB with the RFC 1924 alphabet
   (0-9A-Za-z!#$%&()*+-;<=>?@^_`{|}~; Python base64.b85decode)   -> 68 bytes
2. XOR byte i with "657f48f84c437cc1"[i % 16] (ASCII bytes)        -> zlib stream (78 9C …)
3. zlib-inflate                                                    -> the 64-char passphrase above
```

The same scheme with a different blob yields rekordbox's local `master.db` key
(`402fd482c38817c35ffa8ffb8c7d93143b749e7d315df7a81732a1ff43608497`) [FF] [GIST]. That key does **not**
open `exportLibrary.db`. [GIST] notes Rekordlocksmith (macOS key extraction) and the older
<https://github.com/liamcottle/pioneer-rekordbox-database-encryption> as background.

### 1.3 Creating a file that matches rekordbox's

```sql
-- with SQLCipher 4.x (defaults), before any other statement:
PRAGMA key = 'r8gddnr4k847830ar6cqzbkk0el6qytmb3trbbx805jm74vez64i5o8fnrqryqls';
PRAGMA journal_mode = WAL;          -- rekordbox's files are WAL-mode
-- CREATE TABLE … exactly as §2 …; INSERT …;
PRAGMA wal_checkpoint(TRUNCATE);    -- [FF]: checkpoint after writing
```

* Writer gotcha: if you copy only `exportLibrary.db` and leave an un-checkpointed `-wal` behind (or drop it),
  you lose data. rekordbox's own pristine output carried a 3-frame `-wal` with committed pages
  [ME]. Either checkpoint (truncate) and ship no `-wal`/`-shm`, or ship all three files consistently.
  Whether players need write access to create `-shm` for a WAL-mode file is **UNCERTAIN**. They do write to the
  DB (history, see §5), so sticks are mounted read-write.

---

## 2. Schema: exact DDL from a real rekordbox file

Dumped from `sqlite_master` of the decrypted rekordbox 7.2.18 file [ME]; identical in the other two
fixtures. 22 tables and 4 indexes. There are **no** `NOT NULL`, `FOREIGN KEY`, `UNIQUE` or `DEFAULT` clauses.
Types are `integer`/`varchar` (SQLite affinity only). The names are case-sensitive as shown, including
the odd `OutFileOffsetInBlock` and `isComplation`.

```sql
CREATE TABLE album(album_id integer primary key, name varchar, artist_id integer, image_id integer, isComplation integer, nameForSearch varchar);
CREATE TABLE artist(artist_id integer primary key, name varchar, nameForSearch varchar);
CREATE TABLE category(category_id integer primary key, menuItem_id integer, sequenceNo integer, isVisible integer);
CREATE TABLE color(color_id integer primary key, name varchar);
CREATE TABLE content(content_id integer primary key, title varchar, titleForSearch varchar, subtitle varchar, bpmx100 integer, length integer, trackNo integer, discNo integer, artist_id_artist integer, artist_id_remixer integer, artist_id_originalArtist integer, artist_id_composer integer, artist_id_lyricist integer, album_id integer, genre_id integer, label_id integer, key_id integer, color_id integer, image_id integer, djComment varchar, rating integer, releaseYear integer, releaseDate varchar, dateCreated varchar, dateAdded varchar, path varchar, fileName varchar, fileSize integer, fileType integer, bitrate integer, bitDepth integer, samplingRate integer, isrc varchar, djPlayCount integer, isHotCueAutoLoadOn integer, isKuvoDeliverStatusOn integer, kuvoDeliveryComment varchar, masterDbId integer, masterContentId integer, analysisDataFilePath varchar, analysedBits integer, contentLink integer, hasModified integer, cueUpdateCount integer, analysisDataUpdateCount integer, informationUpdateCount integer);
CREATE TABLE cue(cue_id integer primary key, content_id integer, kind integer, colorTableIndex integer, cueComment varchar, isActiveLoop integer, beatLoopNumerator integer, beatLoopDenominator integer, inUsec integer, outUsec integer, in150FramePerSec integer, out150FramePerSec integer, inMpegFrameNumber integer, outMpegFrameNumber integer, inMpegAbs integer, outMpegAbs integer, inDecodingStartFramePosition integer, outDecodingStartFramePosition integer, inFileOffsetInBlock integer, OutFileOffsetInBlock integer, inNumberOfSampleInBlock integer, outNumberOfSampleInBlock integer);
CREATE TABLE genre(genre_id integer primary key, name varchar);
CREATE TABLE history(history_id integer primary key, sequenceNo integer, name varchar, attribute integer, history_id_parent integer);
CREATE TABLE history_content(history_id integer, content_id integer, sequenceNo integer);
CREATE TABLE hotCueBankList(hotCueBankList_id integer primary key, sequenceNo integer, name varchar, image_id integer, attribute integer, hotCueBankList_id_parent integer);
CREATE TABLE hotCueBankList_cue(hotCueBankList_id integer, cue_id integer, sequenceNo integer);
CREATE TABLE image(image_id integer primary key, path varchar);
CREATE TABLE key(key_id integer primary key, name varchar);
CREATE TABLE label(label_id integer primary key, name varchar);
CREATE TABLE menuItem(menuItem_id integer primary key, kind integer, name varchar);
CREATE TABLE myTag(myTag_id integer primary key, sequenceNo integer, name varchar, attribute integer, myTag_id_parent integer);
CREATE TABLE myTag_content(myTag_id integer, content_id integer);
CREATE TABLE playlist(playlist_id integer primary key, sequenceNo integer, name varchar, image_id integer, attribute integer, playlist_id_parent integer);
CREATE TABLE playlist_content(playlist_id integer, content_id integer, sequenceNo integer);
CREATE TABLE property(deviceName varchar, dbVersion varchar, numberOfContents integer, createdDate varchar, backGroundColorType integer, myTagMasterDBID integer);
CREATE TABLE recommendedLike(content_id_1 integer, content_id_2 integer, rating integer, createdDate integer);
CREATE TABLE sort(sort_id integer primary key, menuItem_id integer, sequenceNo integer, isVisible integer, isSelectedAsSubColumn integer);
CREATE INDEX index_hotCueBankList_cue_hotCueBankList_id on hotCueBankList_cue(hotCueBankList_id);
CREATE INDEX index_myTag_content_content_id on myTag_content(content_id);
CREATE INDEX index_myTag_content_myTag_id on myTag_content(myTag_id);
CREATE INDEX index_playlist_content_playlist_id on playlist_content(playlist_id);
```

---

## 3. Column semantics and the values rekordbox writes

"obs" = observed in the pristine rekordbox 7.2.18 file (34 tracks) and/or the anonymised rekordbox
7.x stick (1644 tracks) [ME]. Descriptions come from [PRB], [SB], [FF] where noted.

### 3.1 `content` (one row per track)

| Column | Meaning / encoding | obs |
|---|---|---|
| content_id | integer PK, 1..n. **Independent of the `export.pdb` track id** [SB] (same file: pdb 578 vs OneLibrary 566). Match the two by `path` | 1.. |
| title | track title | |
| titleForSearch | search key | NULL in every row seen |
| subtitle | | `''` |
| bpmx100 | BPM × 100 | 12400, 13000 |
| length | duration in **seconds** (the [PRB] docs say ms; that is wrong: a 45 s file has 45) | 45, 356 |
| trackNo, discNo | integers (0 when unknown) | 1.., 0/1 |
| artist_id_artist, artist_id_remixer, artist_id_originalArtist, artist_id_composer, artist_id_lyricist | → `artist.artist_id`; NULL or 0 when none ([RBOX] misnames the third `originalartist_id`) | |
| album_id, genre_id, label_id, key_id | → album/genre/label/key | |
| color_id | → `color.color_id` (1..8); **0 = none** | 0 |
| image_id | → `image.image_id` (artwork) | |
| djComment | DJ comment | `''`, text |
| rating | 0..5 | 0 |
| releaseYear | integer | 2020 |
| releaseDate | text | `''` |
| dateCreated, dateAdded | `'YYYY-MM-DD'` text | `'2026-09-23'` |
| path | stick-root-relative audio path, forward slashes, e.g. `/Contents/Artist/Album/a01.mp3`. Same string as PDB / ANLZ PPTH / directory hash | |
| fileName | basename | `a01.mp3` |
| fileSize | bytes | |
| fileType | 0 invalid, 1 MP3, 3 MP4, 4 AAC/M4A, 5 FLAC, 6 ALAC, 11 WAV, 12 AIFF, 16 video ([PRB] master.db docs; 1 and 4 seen) | 1, 4 |
| bitrate | kbps | 320 |
| bitDepth | bits (16 even for MP3) | 16 |
| samplingRate | Hz | 44100 |
| isrc | | `''` |
| djPlayCount | play count | 0 |
| isHotCueAutoLoadOn | 1/0 | 1 |
| isKuvoDeliverStatusOn | 1/0 | 1 |
| kuvoDeliveryComment | | `''` |
| masterDbId | rekordbox `master.db` DBID (constant per library) | 2460304351 |
| masterContentId | the track's id in `master.db` | 65617607 |
| analysisDataFilePath | `/PIONEER/USBANLZ/Pppp/hhhhhhhh/ANLZ0000.DAT` (the `.DAT`; `.EXT`/`.2EX` are siblings) | |
| analysedBits | bit field, meaning **UNKNOWN**; only 105 (`0b1101001`) and 41 (`0b0101001`) seen | 105, 41 |
| contentLink | **UNKNOWN**; identical for every row of one export (`1902336` = 0x1D0700; `853760` = 0x0D0700) | |
| hasModified | | 0 |
| cueUpdateCount, analysisDataUpdateCount, informationUpdateCount | counters, **UNKNOWN** semantics; integer or NULL in 7.2.18 (e.g. 1, 3, 2); one older stick had `''` text in cueUpdateCount | |

### 3.2 `cue` (mirror of the ANLZ cues)

| Column | Meaning | obs |
|---|---|---|
| cue_id | PK | |
| content_id | → content | |
| kind | **0 = memory cue/loop, 1..8 = hot cue A..H** (confirmed against real data and export.pdb [SB]) | 0..8 |
| colorTableIndex | colour, mapping **UNKNOWN** (no palette table in the DB) | 0 in all 69 real rows |
| cueComment | text | NULL |
| isActiveLoop | 1 for the active loop (= PCPT status 4) | 0 |
| beatLoopNumerator / beatLoopDenominator | quantized loop size (= PCP2 fields) | NULL |
| inUsec / outUsec | position in µs. Real rows are ms × 1000. For a non-loop, `outUsec == inUsec` | 3352000 |
| in150FramePerSec / out150FramePerSec | half-frame (1/150 s) position (see anlz.md §2) | NULL in all real rows |
| inMpegFrameNumber, inMpegAbs, inDecodingStartFramePosition, inFileOffsetInBlock, inNumberOfSampleInBlock (+ `out…`/`Out…`) | seek helpers for MPEG/VBR. In master.db the "MPEG frame" values are ~1/75 s units, not real MPEG frame indices, and are 0 for non-VBR [PRB] | NULL in all real rows |

* **Players take cues from the ANLZ files, not this table** ([SB], CDJ-3000X measured 2026-10-02/03).
  When the table and the files disagreed, the player showed the file. A track with no `cue` rows showed
  the file's cues, and specifically the **legacy PCOB** list (anlz.md §4.5). The OMNIS-DUO wrote pads saved on the
  player into the analysis file only. It left `cue` untouched and did not bump
  `cueUpdateCount`. Fill this table anyway (rekordbox does, and rekordbox desktop may read it on import;
  **UNCERTAIN**).
* No foreign keys are declared and none are enforced. To delete a track, delete in this order:
  `hotCueBankList_cue` → `cue` → `playlist_content` → `content` [SB].

### 3.3 Tree and lookup tables

* **playlist**: `attribute` 0 = playlist, **1 = folder** ([SB] fixture expected.tsv);
  `playlist_id_parent` 0 = root; `sequenceNo` 0-based position among siblings; `image_id` NULL.
  playlist_content: `(playlist_id, content_id, sequenceNo)` with sequenceNo 1-based in real data.
* **history / history_content**: same shape. Players add rows when tracks are played [SB]
  (OMNIS-DUO). [MAN]: a new History list is created per session on the stick.
* **hotCueBankList / hotCueBankList_cue**: rekordbox "Hot Cue Bank Lists", same tree shape; `cue_id` → cue.
* **myTag / myTag_content**: 4 fixed root "columns" `(1,'Genre'),(2,'Components'),(3,'Situation'),(4,'Untitled Column')`
  with `attribute = 1`, `myTag_id_parent = 0`, sequenceNo 0..3. Tags are children (`attribute 0`,
  parent = column id) with large 32-bit ids taken from master.db.
* **color**: fixed 8 rows: `1 Pink, 2 Red, 3 Orange, 4 Yellow, 5 Green, 6 Aqua, 7 Blue, 8 Purple`
  (same ids as the ANLZ PCP2 `color_id` and the PDB colour table). rxbox keeps a track's
  colour as a hue and writes the id nearest to it, as in `export.pdb` (docs/pdb.md, colors).
* **key**: `(key_id, name)`, only keys in use. Names are as rekordbox displays them (`'Am'`, `'F#m'`, `'Ebm'`, …).
* **image**: `path` like `/PIONEER/Artwork/00001/b1.jpg` (OneLibrary uses the **`b`** prefix, while `export.pdb`
  artwork rows use `a…` [FF]).
* **artist/album/genre/label**: `name`; `nameForSearch` NULL in all rows seen; `album.isComplation` 0/1.
* **property** (one row): `deviceName ''`, `dbVersion '1000'` (text), `numberOfContents` (= rows in content),
  `createdDate 'YYYY-MM-DD'`, `backGroundColorType 0` (presumably the device-icon BACKGROUND COLOR players offer [MAN];
  mapping **UNKNOWN**), `myTagMasterDBID` (32-bit, from master.db).
* **recommendedLike**: `(content_id_1, content_id_2, rating, createdDate integer)`. Empty in all files seen.

### 3.4 Browse-menu tables: copy these rows verbatim

From the pristine rekordbox 7.2.18 file [ME]. Names are wrapped in U+FFFA … U+FFFB.

```sql
INSERT INTO menuItem VALUES
 (1,128,'￺GENRE￻'),(2,129,'￺ARTIST￻'),(3,130,'￺ALBUM￻'),(4,131,'￺TRACK￻'),(5,133,'￺BPM￻'),
 (6,134,'￺RATING￻'),(7,135,'￺YEAR￻'),(8,136,'￺REMIXER￻'),(9,137,'￺LABEL￻'),(10,138,'￺ORIGINAL ARTIST￻'),
 (11,139,'￺KEY￻'),(12,141,'￺CUE￻'),(13,142,'￺COLOR￻'),(14,146,'￺TIME￻'),(15,147,'￺BITRATE￻'),
 (16,148,'￺FILE NAME￻'),(17,132,'￺PLAYLIST￻'),(18,152,'￺HOT CUE BANK￻'),(19,149,'￺HISTORY￻'),
 (20,145,'￺SEARCH￻'),(21,150,'￺COMMENTS￻'),(22,140,'￺DATE ADDED￻'),(23,151,'￺DJ PLAY COUNT￻'),
 (24,144,'￺FOLDER￻'),(25,161,'￺DEFAULT￻'),(26,162,'￺ALPHABET￻'),(27,170,'￺MATCHING￻');
INSERT INTO category VALUES   -- (category_id, menuItem_id, sequenceNo, isVisible)
 (1,1,0,0),(2,2,1,1),(3,3,2,1),(4,4,3,1),(5,17,5,1),(6,5,0,0),(7,6,0,0),(8,7,0,0),(9,8,0,0),(10,9,0,0),
 (11,10,0,0),(12,11,4,1),(15,13,0,0),(17,24,9,1),(18,20,7,1),(19,14,0,0),(20,15,0,0),(21,16,0,0),
 (22,19,6,1),(23,18,0,0),(26,27,8,1),(27,22,10,0);
INSERT INTO sort VALUES       -- (sort_id, menuItem_id, sequenceNo, isVisible, isSelectedAsSubColumn)
 (0,25,1,1,0),(1,26,2,1,0),(2,2,3,1,0),(3,3,4,1,0),(4,5,5,1,0),(5,6,6,1,0),(6,1,0,0,0),(7,21,0,0,0),
 (8,14,0,0,0),(9,8,0,0,0),(10,9,0,0,0),(11,10,0,0,0),(12,11,7,1,0),(13,15,0,0,0),(15,13,0,0,0),
 (16,23,0,0,0),(17,22,0,0,0);
INSERT INTO color VALUES (1,'Pink'),(2,'Red'),(3,'Orange'),(4,'Yellow'),(5,'Green'),(6,'Aqua'),(7,'Blue'),(8,'Purple');
```

The category/sort rows reflect the user's rekordbox "Category"/"Sort" preferences. [RBOX]'s migration has the
same menuItem rows but `category` 23 → `(18,11,1)` and 27 → `(22,10,1)`, i.e. a different user's preferences.

* [RBOX]'s writer schema differs from the real one. It has NOT NULL constraints, FOREIGN KEYs pointing at non-existent
  `id` columns (fatal with its own `PRAGMA foreign_keys=ON`), `originalartist_id` instead of
  `artist_id_originalArtist`, **no `djPlayCount`**, `dbVersion INTEGER`, and composite PKs on the junction tables.
  Do not use it as the reference. Use §2.

---

## 4. How it relates to the rest of the stick

What rekordbox 7 writes (sources: [FF], [BK], [SB] fixture layout, crate-digger exports doc):

```
/PIONEER/rekordbox/export.pdb              Device Library (DeviceSQL), legacy players
/PIONEER/rekordbox/exportExt.pdb           Device Library extension (My Tag: tag + tag_track tables)
/PIONEER/rekordbox/exportLibrary.db        OneLibrary (SQLCipher 4, WAL mode)
/PIONEER/rekordbox/exportLibrary.db-wal    (as rekordbox leaves it)
/PIONEER/rekordbox/exportLibrary.db-shm
/PIONEER/USBANLZ/Pppp/hhhhhhhh/ANLZ0000.DAT|.EXT|.2EX   shared by both libraries (anlz.md)
/PIONEER/Artwork/00001/a<N>.jpg, a<N>_m.jpg            export.pdb artwork (80×80, 240×240)
/PIONEER/Artwork/00001/b<N>.jpg (+ b<N>_m.jpg?)        OneLibrary `image.path` (the _m variant is **UNCERTAIN**; [FF] writes it)
/PIONEER/MYSETTING.DAT, MYSETTING2.DAT, DJMMYSETTING.DAT, DEVSETTING.DAT   (settings.md)
/Contents/<Artist>/<Album>/<file>          audio (paths stored NFC)
```

* Both libraries point at the **same** ANLZ files and audio paths. `content.analysisDataFilePath` must be
  the hashed directory (anlz.md §1.1).
* rekordbox reports "library inconsistency" when `export.pdb` and `exportLibrary.db` disagree. Players
  of one generation show one library and the others show the other [BK] [AT-G]. Writing a new
  `export.pdb` while leaving an old `exportLibrary.db` (or the reverse) gives a stale library on the
  other player class. [BK] therefore **deletes** `exportLibrary.db*` and `exportExt.pdb` when it writes only the legacy DB.
* Players write to the stick: history rows in OneLibrary, cue pads in ANLZ files [SB]; `export.pdb.bak`,
  `RBFLTR.DAT`, re-analysis files [FF]. Do not mount the stick in rekordbox to "check" it: rekordbox
  rewrites device libraries it opens (it removed 768 of 5825 playlist entries in one rbsync measurement)
  and adds an `exportLibrary.db` [BK].
* File systems [AT-G]: FAT32 everywhere. exFAT only on CDJ-3000X, CDJ-3000, XDJ-AZ, OPUS-QUAD, OMNIS-DUO,
  XDJ-XZ, XDJ-RX3. HFS+ on Mac only. [BK]: NXS2 and older need FAT32 + **MBR** (no GPT).

---

## 5. Which devices read which library

| Source | OneLibrary (`exportLibrary.db`) | Device Library (`export.pdb`) |
|---|---|---|
| [AT-G] official guide (rekordbox ≥ 7.2.11, 2026) | "next-generation players: CDJ-3000X, XDJ-AZ, OPUS-QUAD, etc." | "legacy players: CDJ-3000, XDJ-AX [sic], CDJ-2000NXS2, etc." |
| [AT-A] announcement (Oct 2025) | compatible hardware: CDJ-3000X, **CDJ-3000**, XDJ-AZ, OPUS-QUAD, OMNIS-DUO; software: djay Pro (export now), Traktor Play / Traktor Pro 4 (announced) | – |
| [PRB] docs (2025) | OPUS-QUAD, OMNIS-DUO, XDJ-AZ | – |
| [BK] | needed by CDJ-3000X, XDJ-AZ, OPUS-QUAD, OMNIS-DUO | CDJ-3000, CDJ-2000NXS2, XDJ-XZ and older |
| [FF] | CDJ-3000X, XDJ-AZ, OPUS-QUAD, OMNIS-DUO, "and newer firmware on CDJ-3000" | CDJ-3000, XDJ-XZ, all older |
| [GIST] | "CDJ-3000 (firmware 3.19) uses OneLibrary" | |

**CONFLICT / UNCERTAIN**: the CDJ-3000 (non-X). The guide calls it a Device Library player; the
announcement, [FF] and [GIST] say some firmware also reads OneLibrary. Nobody documents which library a CDJ-3000 prefers
when both exist. Write both, consistently.

Measured behaviour [SB]: CDJ-3000X takes **playlists and track identity from `exportLibrary.db`**
(a playlist entry present only in OneLibrary was shown) and **cues from the ANLZ file's legacy PCOB
lists**. OMNIS-DUO writes stored pads into ANLZ files and play history into OneLibrary.
Beat grids and waveforms always come from the ANLZ files; OneLibrary has no tables for them.

Old players (CDJ-2000NXS2, XDJ-RX2, …) ignore `exportLibrary.db` entirely.

---

## 6. Does anyone write it successfully from scratch?

As of these sources (Oct 2026): **no open project documents a from-scratch `exportLibrary.db` that was
verified on a OneLibrary-only player** (CDJ-3000X / XDJ-AZ / OPUS-QUAD / OMNIS-DUO).

| Project | Write capability | Hardware evidence |
|---|---|---|
| Seabass [SB] | edits an **existing rekordbox-written** DB (cue table, playlist membership, deletions) with best-effort SQLCipher | CDJ-3000X read playlists from OneLibrary. Cue edits there were ignored by the player (cues come from ANLZ) |
| fourfour [FF] | full from-scratch writer (22 tables, WAL checkpoint, menu defaults) | "proven on a CDJ-3000", which also reads `export.pdb`. No published OneLibrary-only-player result |
| rbox [RBOX] | `OneLibrary::create()` from its own migration | none; its schema deviates (§3.4) |
| pyrekordbox [PRB] | SQLAlchemy ORM on an existing DB | none |
| onelibrary-connect [OLC], beat-link/crate-digger [BL] | read-only | – |
| baken [BK] | none ("not supported yet"); deletes stale OneLibrary files | – |
| libdjinterop #177, Mixxx #15556 [GIST] | open issues | – |

---

## 7. Writer checklist

1. Write `export.pdb` **and** `exportLibrary.db` describing the same tracks and playlists; the same ANLZ files
   and the same `/Contents/...` paths. Ids in the two DBs are independent.
2. SQLCipher 4 defaults, passphrase from §1.2, `journal_mode=WAL`, exact DDL from §2 (no extra constraints,
   same column names and order), the four indexes.
3. Fixed rows: `color` 1..8, `menuItem` 1..27, `category`, `sort` (§3.4); `myTag` roots 1..4; one `property` row with
   `dbVersion '1000'` and `numberOfContents` = content row count.
4. `content.path` = stick path; `analysisDataFilePath` = hashed `.DAT` path; dates `'YYYY-MM-DD'`; `length`
   in seconds; `bpmx100`; `color_id` 0 = none; `isHotCueAutoLoadOn 1`.
5. Cue rows mirror the ANLZ cues: `kind` 0/1..8, `inUsec = ms*1000`, `outUsec = inUsec` for points, legacy
   frame columns NULL (as rekordbox 7 leaves them). But **put the authoritative cues in ANLZ PCOB+PCO2**.
6. Playlists: folders `attribute 1`, root parent 0, sibling `sequenceNo` from 0; entries `sequenceNo` from 1.
7. Checkpoint the WAL before unmounting. Don't open the stick in rekordbox afterwards.
8. Unknowns you must fill: `analysedBits` (use 105), `contentLink` (copy a rekordbox value, e.g. 0x1D0700, or 0;
   **UNCERTAIN** what players do with it), `masterDbId`/`masterContentId` (any stable 32-bit values; no evidence that players
   read them), update counters (rekordbox 7.2.18 wrote NULL/NULL/NULL on its 22 rows with analysedBits 41 and
   `1, 3, 2` on its 12 rows with analysedBits 105; meaning **UNKNOWN**).
