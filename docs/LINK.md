# PRO DJ LINK: serving the library to players (plan)

Status: a plan, nothing implemented. What rekordbox calls **Link Export**:
the computer joins the players' network as `rekordbox`, appears on their
SOURCE screen, and a player browses rxbox's library, loads a track and
plays it with its grid, cues and waveforms, with no stick. The players
also say what they have loaded and whether it plays, which rxbox can show
and count as plays.

The players: those with an Ethernet port that speak standard PRO DJ LINK,
such as the CDJ-3000 and the CDJ-3000X, over Ethernet and nothing else.

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

- **Joining.** Nothing is announced into an empty network: the link comes
  up on the first keep-alive heard from a player or mixer. Then three
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
- rekordbox exports the whole root `/`. rbxport allows only the library's
  folders: each track's path cut at its third component, so a
  38 000-track library comes to a handful of roots. rxbox does the same.

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

### 4. Files players cannot decode

A player reads the file as it is. What the export transcodes is served
the same way: a codec no player reads (`rx_plan_codec_playable` refuses
it: Opus, Vorbis …) as FLAC, and with the compatibility transcode on, AAC
and hi-res FLAC as 16-bit FLAC. The file named is the export's transcode
in the library's cache (`rx_plan_transcode`), made when Link starts for
the tracks that need one, or at the latest before the track-info reply
names it.

## In rxbox

- **`src/link`**, a library like the others: the codecs (packets,
  database messages, XDR/RPC/NFS) as pure functions, and one service
  thread polling the sockets.
- **The library on the service thread**: its own connection and its own
  `RXLibrary`, as the export job opens one, loaded again when the app says
  the library changed. The app owns the library: what players send back
  (loads, plays; later cues) goes to the app as job results, the way the
  export's do.
- **Off by default**, bound to one chosen interface (`link.interface`):
  a Link switch and panel in the interface (the players seen, what each
  has loaded, playing or not), and `rxbox link [--interface IF]` running
  the server in the foreground from the CLI.
- **Exposure**: read-only, only the library's folders, only the chosen
  interface. A host firewall must let UDP 50000, 50002, 50111 and 2049
  and the TCP database ports in on that interface.

## Order of work

1. **A hardware check before any code** (an hour): a CDJ-3000 or
   CDJ-3000X on Ethernet beside rekordbox: what does `tcpdump` see on that
   interface?
2. **The codecs**, with their tests: packets, database messages, XDR, RPC
   and NFS, checked against captured bytes and the published layouts.
3. **Presence**: join, keep-alive and status, tested against an in-tree
   fake player on loopback; on hardware, rxbox appears on SOURCE.
4. **Load and play**: the database server's root menu, all tracks,
   playlists, metadata, artwork, track info, grid, waveforms and cues, and
   the NFS server. On hardware, a track loads and plays.
5. **The rest of browsing**: categories, search, sort, history, keys, the
   after-load requests.
6. **Status in rxbox**: the Link panel; plays counted from the players'
   status.
7. **Later**: writes from the deck (cues, grid), several interfaces.

Roughly 12 000 to 15 000 lines of C with the tests; steps 2 to 4 are the
first half and give the first track playing from rxbox.

## Tests

- The codecs: round trips and the captured bytes as fixed vectors.
- Loopback integration: an in-tree fake player (as rbxport's
  `rbl-fakecdj`) that joins, browses, asks for track info, mounts and
  reads a file over NFS and compares the bytes.
- Hardware: a checklist like rbxport's `link-testing.md` catalog, per
  model and firmware, the visible result recorded apart from the packets.

## Open questions

- Whether players take a library they did not see on the network before
  as reliably as rekordbox (rbxport: yes on a CDJ-3000 in the emulator).
- How much of the browse menu tree players depend on, against what can
  be left out at first.
