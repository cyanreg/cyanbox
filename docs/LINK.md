# PRO DJ LINK: serving the library to players

What rekordbox calls **Link Export**:
the computer joins the players' network as `rekordbox`, appears on their
SOURCE screen, and a player browses rxbox's library, loads a track and
plays it with its grid, cues and waveforms, with no stick. The players
also say what they have loaded and whether it plays, which rxbox can show
and count as plays.

The players: those with an Ethernet port that speak standard PRO DJ LINK,
such as the CDJ-3000 and the CDJ-3000X, and the older ones down to the
CDJ-900, over Ethernet and nothing else.

## Status (2026-10-14)

Steps 1 to 6 of the order of work below are written and tested without a
player: `src/link` serves the library on every Ethernet interface, as
they come and go, from the CLI (`rxbox link serve`) and from the
interface for as long as it runs with `--pro-link` (the PRO DJ LINK
window, src/app/link.c and src/ui/link_view.cpp), the interface counts a
track of ours that a player plays for 30 s as a play,
and `rxbox link browse -i IF [HOST]` is a fake CDJ-3000 that joins,
browses, loads a track and reads it as a player does. Step 7 has begun:
two CDJ-900s on Ethernet (2026-10-14) joined, mounted the library,
browsed it and played its MP3s from it. Not yet: a WAV played on one
(section 4), a CDJ-3000, writes from the deck (step 8).

What the CDJ-900s showed, and what changed for it (`rxbox-cli -v link
serve` logs every request, NFS call and unknown packet):

- Announcing to the interface's broadcast address as the kernel reported
  it sent everything to ourselves when the address had been added
  without one (glibc then gives the address itself): only the subnet's
  own broadcast address is taken now (src/link/net.c).
- They set up the older way (one argument), take twelve-argument rows,
  send 0xd0-byte statuses with no beat number, and read files in 2 kB
  NFS READs.
- The track info reply's first row names the file's type, and a player
  decodes the file as that (section 4).
- Scrolling a list, a player previews the row under the cursor and gives
  the preview up with `0001` and that request's txid; a CDJ-2000nexus
  answers nothing to it (dysentery's captures), and now rxbox neither.

Then emulated players running Pioneer's own firmware (a CDJ-900 v4.32, a
CDJ-2000 v4.33, a CDJ-2000NXS2 v1.87; 2026-10-15) showed:

- A deck that drops off the device list (no keep-alive for 10 s: a deck
  busy loading, a switch that rebooted, a cable reseated) and comes back
  mid-load reads on and never mounts again; refused, an NXS2 retried the
  READ 43 times a second for minutes and never finished loading. Now an
  address that was on the link is answered for half an hour after it
  dropped off, its mount is kept, and a deck that mounted in this session
  and reads with a handle of it is taken back in (nfs.c, may_read_node).
- Text in NFD (a kana and its dakuten apart, as tags written on macOS
  hold it) reached a CDJ-2000 as two characters. The library keeps all
  its text in NFC now (src/common/utf8.c; existing text is converted as
  it opens, src/core/clean.c), so the stick and Link give out NFC.
- A CDJ-900 shows Latin-1 only, everything else as '?': "Don’t" as
  "Don?t", kana and kanji all '?' (its main processor hands the display
  8-bit names). For its sessions (by the keep-alive's name) every text
  goes as Latin-1 (rx_dbmsg_item_latin1, rx_utf8_latin1): Japanese as
  romaji, kanji read by MeCab when rxbox has it and a dictionary
  ("悦びの印" as "Yorokobi no Shirushi", "ごーいん！" as "Gooin!"),
  typographic and CJK punctuation as its nearest Latin-1, other scripts
  through ICU. Names with readings of their own come out by their
  ordinary readings (小鳥遊 is not "Takanashi"); a kanji the dictionary
  has no reading for gets its on'yomi from Unicode's Unihan ("秋奈" as
  "Akina", "夜一夜の戰" as "Yoru Ichiya no Sen"). Rows are cut at 63
  bytes by the player itself (its main processor's limit).
- A CDJ-900 accepts the WAV made as it is read (16-bit stereo, 44.1 or
  48 kHz, the plain 44-byte header): a LOOKUP per path component, a
  GETATTR, 2 kB at 0 for the header, then 9408-byte READs (four CD
  sectors). The emulated one went on to read whole tracks at once, even
  ones it had not loaded; that is the emulation, whose DSP model reports
  no buffer levels, not the player (a real one has 64 MB of memory, less
  than a long WAV). A player streams a track as it plays it: an emulated
  CDJ-2000NXS2, whose DSP code is the real one, read about 630 kB in the
  first 2 s after a load, then at about the rate it plays (140 to 180
  kB/s), and nothing of the tracks after it.
- The older cue list (`0x2104`) gives a pre-nexus deck its memory cues
  and hot cues A to C (a hot loop D is left out: those decks have three).
  A CDJ-900 has no hot cue keys at all (its firmware's key table marks
  them "2000 only") and ignores them; a CDJ-2000 has them.
- A CDJ-2000's key menu has two levels of its own: `0x100b`, the keys
  (not `0x1014`), and `0x110b`, a key's tracks (given the related keys'
  rows, it took them for a leaf and asked nothing more). It reads a WAV
  as the CDJ-900 does. FILE NAME lists by the file's name now, not the
  title.

Controlling the players (2026-10-15, beat-link's packets sent to the
emulated players): the load command (`19` to the player's status port:
load track N of device D's slot S, as rekordbox does for a track dragged
onto a deck) worked on the CDJ-900 and the CDJ-2000NXS2, which answers
with a `1a`; rxbox sends it (`rx_link_load`, the interface's Load in
deck), and its fake player obeys it. Fader start (`02` on 50001) did
nothing on either; the on-air flags (`03`) only on the NXS2. Nothing else
of a deck can be worked from the network: not its play button, pitch,
jog, cues or loops.

What has been run against what:

| Server | Client | Network | Result |
| --- | --- | --- | --- |
| rxbox | two CDJ-900s | Ethernet, a switch, 169.254.0.0/16 | join, mount, browse, the library's MP3s played; WAVs decoded as MP3 until the file type was named |
| rxbox | emulated CDJ-900, CDJ-2000, CDJ-2000NXS2 (Pioneer's firmware in QEMU) | a bridge, 169.254.0.0/16 | join, mount, browse every category, metadata, waveforms, cues, a load streamed over NFS; the lapse, NFD, CDJ-900 punctuation and CDJ-2000 KEY findings above; FLAC, MP3, AAC and 16- and 24-bit WAV sources played on the NXS2, with tempo, jog, memory loops, and hot cues A to D in their colours (the pads jump, the hot loop loops); the load command (`0x19`) on the CDJ-900 and the NXS2; no cue writes attempted from an NXS2 |
| rxbox | rxbox's fake player | loopback, ports of their own (`test_loopback`) | join, browse, every track's bytes (transcodes too), the player's status, the fences, a library change |
| rxbox | rxbox's fake player | two network namespaces, rekordbox's ports (`tests/link/netns_test.sh`) | the same through the CLI, real broadcasts; the Ethernet interface found by the server, two served at once on the same ports, one of them only once its address came |
| rxbox's app | rxbox's fake player | loopback (`test_link`) | refused without `--pro-link`, on the link before any player, the player and its load seen, 30 s of play recorded, the library read again after it, off, off right after on |
| rxbox's interface | rxbox's fake player | two network namespaces, by hand | the window: on the link as 17, the player's load by title, playing, its play counted |
| rxbox | rbxport's `browse` example | two network namespaces | root, sort, tracks, playlists, metadata, track info, grid, preview, the file over NFS |
| rbxport's `serve` (its emulator fixture) | rxbox's fake player | two network namespaces | the same; rbxport reads our CDJ status (loaded, playing) |

rbxport's client was written against rekordbox and a CDJ-3000 in an
emulator, and its server answers as rekordbox does (its captures); each
side of rxbox agreeing with the other side of rbxport is the check that
rxbox does not only agree with itself. The two servers' replies have the
same shapes; where they differ, rbxport's capture decided (the metadata
rows' order) or the difference is a preference (rekordbox shows the
comment beside a title, rxbox the artist; rekordbox's root menu holds the
categories its user chose).

The codecs' own tests check captured bytes (rbxport's captures of
rekordbox 7.2.11 and a CDJ-3000, an XDJ-700's NFS handles, the
djl-analysis' examples) and run under a fuzzer with AddressSanitizer and
UndefinedBehaviorSanitizer (`fuzz-presence`, `fuzz-db`, `fuzz-nfs`).

**Running it by hand.** `tests/link/netns.sh SERVER -- CLIENT` makes a
private two-host network (user and network namespaces, a veth pair,
169.254.1.1 and .2; no root, nothing reaches another network) and runs a
server on one host and a client on the other. rbxport's examples build
with `CARGO_TARGET_DIR=build/rbxport-target cargo build --release -p
rbl-link --example browse --example serve --example emu_fixture` in its
tree; `emu_fixture DIR` makes its three-track library and
`RBXPORT_OPTIONS=DIR/options.json serve rxa` serves it.

**Known limits.** A track info request for a track whose transcode is not
made yet (with `link.mp3`, or for what cannot be decoded as it is read:
section 4) makes it there and then, on the service thread: the database
and NFS wait for it (seconds), presence does not (its own thread, so the
keep-alives go on), and stopping Link (quitting) waits for it too. HISTORY lists every play
the library recorded, by day (rx_library_plays), and a track played
before the library kept plays by its last play. A player reading a file
is not told when the library changes under it; handles name tracks by
id, so it keeps reading.

## Sources and how far they are proven

- **rbxport** (`~/projects/rbxport`, GPL, Rust): a complete Link Export
  server, read for facts only, as before (no code is copied). Its crates:
  `rbl-prolink` (packets), `rbl-link` (join, beacon, file tree, analysis
  blobs), `rbl-dbserver` (remote database), `rbl-nfs` (NFS), `rbl-fakecdj`
  (a test client). Measured against rekordbox 7.2.11 and a CDJ-3000:
  packet captures, rekordbox's binary, and CDJ-3000 firmware 3.20 booted in
  an emulator (AtEmu), which browses, loads over NFS and plays two tracks.
  `docs/reference/link-testing.md` there marks every behaviour `[OBS]`,
  `[ASSUME]` or `[UNKNOWN]`.
- **Deep Symmetry's djl-analysis** (dysentery, beat-link, crate-digger):
  the community analysis of the packets and the database protocol, from
  captures of real hardware.

## The pieces

Every port is above 1024: nothing needs root.

### 1. Presence (UDP 50000 and 50002)

- **Joining.** At once, when a service starts (`start_up`, the default):
  rxbox has its number and is on the players' SOURCE lists before the
  first of them is switched on, and nothing depends on hearing one first;
  a join that fails is tried again. rekordbox instead waits for the first
  keep-alive of a player or mixer, which rxbox can do too (`rxbox link
  serve --wait`, and the tests of that wait). Then three
  first-stage claims 100 ms apart, a probe of each of the six numbers
  rekordbox reserves (17, 18, 41, 42, 43, 44) every 100 ms, six rounds,
  skipping any a device answers for; rekordbox settles on 17 or 18. Once
  running, a keep-alive with the number every 2 s, and an "in use" answer
  to anyone probing it.
- **Status.** A mixer-style status packet every 200 ms on 50002; answers
  to a player's media query and its device-settings request; a greeting
  the first time a player reports in.
- **Listening.** Every player's status packet says what it has loaded and
  from where (our device number as the source), whether it plays, its
  tempo and position: the Link panel and play counting come from these.
- Every packet starts with the same ten-byte magic, a kind, a subtype and
  a twenty-byte device name; rbxport's tests hold the bytes rekordbox
  7.2.11 sent a CDJ-3000, which are the test vectors here.

### 2. Remote database (TCP; port query on 12523)

A player connects to TCP 12523, asks which port the database server is
on, connects there and exchanges framed messages: a magic, a transaction
id, a type and up to twelve typed arguments (numbers, UTF-16 strings,
binary blobs). Menus come as a header, items and a footer, paged.

| What | Request | Notes |
| --- | --- | --- |
| Session | `0000` setup, `0100` teardown | |
| Root and categories | `1000`, `1001` genre … `1014` key | the categories rekordbox offers |
| All tracks | `1004` | |
| Playlists | `1105` (folder and list modes) | playlist order, first position 1 |
| History | `1012`, `1112` | |
| Search | `1300`; CDJ-3000 live search `1500` | |
| Sort | `1400` | |
| Keys | `1014` related keys | |
| Track metadata | `2002` | title, artist, BPM, key, length … |
| Artwork | `2003` | a no-art status (`4002`), never a timeout |
| Track info (load) | `2102` | the file's absolute path and size, then NFS |
| Waveforms | `2004` preview, `2904` detail, `2c04` an ANLZ tag by name | |
| Beat grid, cues | `2204`; cues `2104` (legacy) and `2b04` (extended) | |
| VBR index | `2504` | |
| After a load | `3006` user info (answered with `4d02`, 160 bytes), `2602` delivery info | the CDJ-3000 waits for these before it goes back to browsing |
| Writes from the deck | `2105` cues, `2705` extended cues, `2605` grid offset … | refused at first; later, saved to the library |

The library is in memory already, so every menu is a query over
`RXLibrary` and the album and collection orders rxbox has. The analysis
replies are the ANLZ tags the export writes (`src/formats`), made in
memory.

### 3. NFS (UDP: portmap 50111, NFS 2049)

The player reads the audio file over NFS, at the path the track-info reply
gave it. What rekordbox serves is not a standard NFS server:

- **Portmap on UDP 50111**, not 111: a player asks rekordbox there. Mount
  version 1 and **NFS version 2** behind it, NFS on 2049.
- **Names are UTF-16LE**, in mount paths and file names alike.
- **Read-only**: every write procedure answers `NFSERR_ROFS`.
- rekordbox exports the whole root `/` and answers any host path; rbxport
  narrows that to the library's folders. rxbox exports nothing of the
  host at all (below, "A virtual tree").

**Can an off-the-shelf NFS server do it? No.**

- *The Linux kernel's nfsd*: its NFS version 2 server is a build option
  that current kernels leave out (`CONFIG_NFSD_V2 is not set` on this
  machine, kernel 7.2). It also needs root, registers with the system's
  portmapper on 111, and knows nothing of UTF-16 names.
- *unfs3* (BSD-3-Clause, user space): NFS version 3 only.
- *NFS-Ganesha*: LGPL-3, versions 3 and 4, a large daemon.
- *libnfs*: a client library (LGPL-2.1), the wrong side.
- *libtirpc* (BSD-3-Clause), the ONC RPC and XDR runtime nfs-utils uses,
  is the one candidate for a part: it could do the framing. It is not
  installed here; its server layer keeps global state and wants to
  register with rpcbind; and the protocol surface is three programs with
  a few procedures each, over UDP. An XDR reader and writer and the RPC
  call and reply headers are about 300 lines of C.

So rxbox writes its own, as rbxport did (its whole NFS crate is about
2 300 lines of Rust with the file tree). In C: about 1 500 lines, written
as pure functions from a request datagram to a reply datagram, testable
without a socket.

**A virtual tree.** A player does not browse the NFS server: it asks the
mount program for its exports, mounts one, and walks, name by name, the
path the database server's track-info reply gave it, then reads the file
(rbxport's test client, `rbl-fakecdj`, and alphatheta-connect's, written
against real players, do exactly this). The path is ours to choose. So
the export is one virtual root whose only contents are the library's
tracks, at the paths the export plan already gives them on a stick
(`RXPlanTrack.usb_path`, `/Contents/Artist/Album/01 Track.flac`), each
file standing for the plan's `source`: the track's file, or its
transcode in the cache (section 4), with the plan's size. No host path
appears in the protocol, and nothing but the library's audio can be
named, so nothing else can be read. A file handle is an index into that
tree with a random per-session tag: a handle from another session is
stale, any other is refused. Reads open the source by the library's own
path, never by a name a player sent.

### 4. Files players cannot decode

A player reads the file as it is, and decodes it as the format the track
info reply names: the second field of its first row is the file type,
rekordbox's numbers (1 MP3, 4 M4A, 5 FLAC, 11 WAV, 12 AIFF). A
CDJ-2000nexus serving its own USB sends 1, 4, 11 and 12 there for an MP3,
an M4A, a WAV and an AIFF (dysentery's `S13-format-ground-truth`
capture); a CDJ-900 told 1 for a WAV reads the last 128 bytes for an
ID3v1 tag, then hunts through the whole file for MPEG frames and never
plays (2026-10-14, two CDJ-900s; rxbox sent 1 for every file until then,
copied from a rekordbox capture of MP3s). The `2f` row after the path is
1 for every format in both captures.

So Link serves every track in one form every Ethernet player reads, from
the CDJ-900 to the CDJ-3000X, and names its type: a 16-bit stereo WAV,
made from the track's own file as a player reads it (src/link/wav.c).
Nothing is made ahead and nothing is written to disk: a read is mapped
onto the frames it covers, the WAV's 44-byte header and 4 bytes a frame,
and those frames are decoded then through the packet store the decks
play from (rxbox/audio.h), which starts any range from enough packets
before it for the decoder to give what a continuous decode gives
(exactly for FLAC, PCM, ALAC and MP3; within 1e-7 for Opus and AAC). So
a player plays the very samples the beat grid and the cues were made on,
whatever the codec: no encoder delay to guess for MP3 or AAC, no VBR
index for a player to get wrong. MP3s are served so too.

- 16-bit lossless sources go through bit for bit; 24-bit, float and
  lossy ones get one step of triangular dither drawn from each sample's
  place, so the same bytes come back however often and in whatever order
  a player reads them. Mono is doubled.
- The WAV is the plan's length, the frames the library counted when it
  probed the file (silence after the file's end, nothing past it): its
  size is known at the track info reply and never changes under a
  player, reloads included.
- On this machine a 2 kB read takes 10 to 16 µs, a 32 kB one 0.15 to
  0.25 ms, a jump (a hot cue) 0.25 to 0.4 ms, the first read of a track
  (opening it) 1 to 4 ms with the file in the page cache, about 200 ms
  from the disk (an emulated CDJ-2000NXS2's first READs: 205 to 227 ms,
  tolerated); a 13-minute FLAC reads through 790 times faster than it
  plays. A few tracks are kept open (1 GiB of packets at most),
  the least recently read closed first.
- What the packet store cannot place sample for sample (Matroska, which
  keeps milliseconds), above 48 kHz or of more than two channels is made
  as a WAV file when a player loads it (`RX_TRANSCODE_LINK`,
  `rx_transcode_wav`, in the library's cache as `ID.link.wav`), as is a
  track the WAV cannot be made of after all (the track info request
  checks: `RX_TRANSCODE_LINK_ANY`).

With the `link.mp3` setting it is a 320 kbps CBR MP3 instead, everything
but an MP3 (`RX_TRANSCODE_LINK_MP3`, `rx_transcode_mp3`), made whole when
a player loads the track: no tags, no cover, no Xing/LAME frame, and the
encoder's delay cut from the start of the audio fed to it, so the file as
a player decodes it lines up with the source and the grid and cues stay
right (`test_transcode` checks the clicks of its signal to 2 samples).
A CDJ-900 played the library's own MP3s at once; the WAVs, now that their
type is named, have played on an emulated one but not yet on a real one.

The USB export is not changed by any of this: what the stick gets is its
own choice (`export.format`, README.md).

## Security

PRO DJ LINK has no authentication and no encryption: the database server
answers any connection, NFS version 2 takes a client's word for who it is
(AUTH_UNIX), and all of it is plain UDP and TCP. That cannot be fixed on
our side without the players' cooperation, so the design keeps what an
attacker on that network can reach to what a player needs, for as long as
Link is on:

- **What can be read**: the library's audio files (the virtual tree), its
  metadata, artwork and analysis, all read-only. No host path, no other
  file, no directory of the host. Writes from players (cues, grid) are
  refused until they are a decision of their own.
- **When**: never without `--pro-link` (or `rxbox link serve`); with it,
  for as long as rxbox runs.
- **Where**: every Ethernet interface that is up with an address, each
  served by a service of its own (no other kind is accepted: not Wi-Fi,
  not a tunnel); a datagram or connection that arrives on another
  interface than its service's is dropped (`IP_PKTINFO`). The best network
  is one with only the players on it: a direct cable or a switch of their
  own.
- **From whom**: the database server and NFS answer only addresses that
  are players on Link (they send keep-alives on 50000), or were in the
  last half hour (a deck back from a lapse of its keep-alives must not
  find itself refused mid-load). UDP sources can be forged, so this is a
  fence against casual access, not authentication; TCP's cannot be, so
  it holds for the database server.
- **What is parsed**: every datagram and message is read by a bounded
  reader that checks each length against what arrived before it reads,
  allocates nothing from a length a peer sent, and answers what it does
  not know with an error. Only the procedures players use exist: portmap
  NULL, GETPORT and DUMP (no SET or UNSET: nothing registers with us),
  mount NULL, MNT, UMNT and EXPORT, NFS NULL, GETATTR, LOOKUP, READ,
  READDIR and STATFS; every write is `NFSERR_ROFS`, everything else
  `PROC_UNAVAIL`. The codecs are pure functions, run under a fuzzer
  (random and mutated datagrams, with AddressSanitizer and
  UndefinedBehaviorSanitizer) as part of the tests.
- **Amplification**: replies to unknown or malformed requests are no
  larger than the request; READ answers only an address that mounted in
  this session, in the subnet, with a handle of the session.

## In rxbox

- **`src/link`**, a library like the others: the codecs (packets,
  database messages, XDR/RPC/NFS) as pure functions, and one service
  thread polling the sockets.
- **The library on the service thread**: its own connection and its own
  `RXLibrary`, as the export job opens one, loaded again when the app says
  the library changed; and the export plan over the whole library
  (`rx_plan_build_cb`), which the database server's paths and the virtual
  tree both come from, made again with it. That plan reads no audio
  (`RX_PLAN_STORED`): each file is described from what the library stored
  when it probed it (a `stat` says it is still that file; one changed
  since is probed), each cover is the library's thumbnail, and the frame
  tables the analysis needs (an MP3's PVBR, a FLAC's PVB2) are read from
  one file when a player asks for its analysis. A plan that probed every
  file, as the export's dry run does, took over a minute on a library of
  1,400 tracks, every time the library changed, and stopping waited for
  it; this one takes 0.4 s, and a stop interrupts it. A handle names a
  track by id, so a player reading a file keeps reading it across a
  rebuild. The app owns the library: what players send back
  (loads, plays; later cues) goes to the app as job results, the way the
  export's do.
- **Every Ethernet interface** (`src/link/set.c`, `RXLinkSet`): a service
  per interface, each bound to its own; they bind the same ports, which
  Linux allows on different devices. A thread of the set's own looks at
  the interfaces every 2 s: a service starts on one that has come up with
  an address (a cable plugged in), stops on one gone or without it, and
  starts again when the address changed; one that would not start (the
  ports held there by another program) is tried again every 10 s.
- **With `--pro-link`**, from the start, for as long as the interface
  runs: the PRO DJ LINK window shows each interface served and the
  devices on them. Without it there is no Link menu and nothing is ever
  bound (rx_app_link_start refuses). `rxbox link serve` does the same in
  the foreground from the CLI (`-i IF`: one interface only).
- **The app's side** (src/app/link.c): the set starts and stops the
  services on its thread (a start reads the library and plans over all
  of it; a stop waits for a request in progress), each tick copies its
  state for the window, and the library is read again 2 s after its last change
  (30 s at most while changes keep coming: an analysis queue). Plays are
  counted there, on the owner thread, from each player's status as the
  deck counts its own: 30 s of distinct audio of one load, a beat at a
  time (60 / BPM seconds each, so a loop counts its beats once and a jump
  adds nothing), or the time it played at its pitch when the player gives
  no beat numbers; then `rx_library_record_play`, once per load.
- **Exposure**: as in "Security". A host firewall must let UDP 50000,
  50002, 50111 and 2049 and the TCP database ports in on that interface.

## Order of work

All of it can be built and tested before a player is at hand: the
protocol facts are published and measured (rbxport's captures, Deep
Symmetry's analysis), and the tests below stand in for the hardware.

1. **The codecs**, with their tests and the fuzzer: packets, database
   messages, XDR, RPC and NFS, checked against captured bytes and the
   published layouts.
2. **The virtual tree** over the export plan, and the NFS server on it.
3. **Presence**: join, keep-alive and status, tested against an in-tree
   fake player on loopback.
4. **Load**: the database server's root menu, all tracks, playlists,
   metadata, artwork, track info, grid, waveforms and cues; the fake
   player browses, loads and reads a track's bytes back over NFS.
5. **The rest of browsing**: categories, search, sort, history, keys, the
   after-load requests.
6. **Status in rxbox**: the Link panel; plays counted from the players'
   status.
7. **With the hardware**: a CDJ-3000 or CDJ-3000X on Ethernet (what does
   `tcpdump` see), then the checklist below.
8. **Later**: writes from the deck (cues, grid), several interfaces.

Roughly 12 000 to 15 000 lines of C with the tests; steps 1 to 4 are the
first half and give the first track loaded from rxbox by the fake player.

## Tests

- The codecs: round trips and the captured bytes as fixed vectors.
- Loopback integration: an in-tree fake player (as rbxport's
  `rbl-fakecdj`) that joins, browses, asks for track info, mounts and
  reads a file over NFS and compares the bytes; and asks for host paths,
  `..`, forged and stale handles, and gets nothing.
- Our server tested only by our own client would agree with itself. So
  the fixtures are other people's bytes: rbxport's captures of rekordbox
  talking to a CDJ-3000, and Deep Symmetry's documented examples; and, as
  a check run by hand, clients written against real players
  (alphatheta-connect's, Deep Symmetry's beat-link) pointed at rxbox.
- The fuzzer over every codec, under the sanitizers.

### Player firmware in an emulator

rbxport's firmware evidence comes from **AtEmu**, an emulator kept
outside its public repository with a private harness (`ATEMU_DIR`,
`rbxport-private`); it is not published. The public one is
**cdj3k-emu** (<https://github.com/nsaintot/cdj3k-emu>, Apache-2.0 or
MIT, some parts GPL-2.0): QEMU `aarch64-virt` with a stock kernel, its
own modules and a shim for what the firmware expects (Rockchip DRM, the
sub-CPU's buttons and jog, audio), booting the real firmware of a
CDJ-3000 (3.00 and later, the RK3399 boards), CDJ-3000X or CDJ-1500X;
its README says "Boots, Plays, Pro DJ Link" for each. What it would mean
here:

- **The firmware and its key are the user's to supply**: the `.UPD`
  update file, which AlphaTheta publishes, and its LUKS keyfile, which it
  does not ("Getting the keyfile is the user's responsibility, by
  whatever means they are entitled to"). Nothing here can come with it.
- **On this machine**: x86_64, so the ARM guest runs under QEMU's
  software emulation (TCG), slower than on an ARM host with KVM; it wants
  a GL 3.3 desktop (the private headless sway the UI tests use), PipeWire,
  and for Pro DJ Link a tap or macvtap made with root once. A tap with
  only the emulator and rxbox on it is also the isolated network the
  Security section asks for.
- **No automation in it**: rbxport's harness drives the panel (buttons,
  jog, touch) and reads the screen; for cdj3k-emu that would be written
  here, through its input and display paths, with a packet capture of
  the same session.
- **Unknown**: its README says "LINK MODE, where rekordbox reaches the
  emulated deck over the network, does not work", without saying whether
  that is Link Export or what fails. rbxport's AtEmu loads tracks from a
  Link Export source on CDJ-3000 firmware 3.20, so the firmware does it;
  whether cdj3k-emu does is the first thing to try.

Used, it would stand in for steps 3 to 5 on a CDJ-3000: rxbox on SOURCE,
browsing, a track loaded over NFS and played, the after-load requests.
- Hardware: a checklist like rbxport's `link-testing.md` catalog, per
  model and firmware, the visible result recorded apart from the packets.

## Open questions

- That a player takes a path that is not a host path (`/Contents/…`): it
  only walks it, by every client written against players, but no player
  has been shown one.
- Whether players take a library they did not see on the network before
  as reliably as rekordbox (rbxport: yes on a CDJ-3000 in the emulator).
- How much of the browse menu tree players depend on, against what can
  be left out at first.
