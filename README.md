# SkyPortal payload (PS5, Skylanders PS4 titles)

A plain PS5 payload ELF. It makes a Skylanders game believe a USB Portal of Power is attached and serves a phone
web UI (default `http://<ps5-ip>:8787`) to pick which 1024-byte figure dump sits on which portal slot.
No physical portal is needed. It is the same core as the etaHEN plugin version (hook blob, injector, API, web UI);
only the entry point and build differ.

## Tested
| | |
|---|---|
| Console | PS5, firmware **13.42** |
| Game | Skylanders **Trap Team** (`CUSA00697`): the game accepts the virtual portal, and figures can be loaded and removed from the web UI |
| Not tested | Swap Force (`CUSA00019`, configured in the sample ini), other regions' CUSA ids, other Skylanders titles |

## Run
1. Copy `config/skyportal.ini` to `/data/skylanders/skyportal.ini` (Swap Force `CUSA00019`, Trap Team `CUSA00697`;
   add a `title=` line for your region's CUSA id).
2. Send `skyportal.elf` to your payload loader (elfldr, usually port 9021), **or** put it in
   `/data/OnionHEN/payloads/` and start it from the Toolbox payload list.
3. A toast shows `SkyPortal ready: http://<your PS5 IP>:8787`. Open that address on a phone and upload your
   1024-byte figure dumps.
4. Start the game. About 25 seconds later (`settle_ms`) a second toast says `SkyPortal: game hooked`.
   Pick figures for the slots on the web page after that.
5. Log: `/data/skylanders/skyportal.log`. Expect `kernel_base=...`, `attaching to ...`, then
   `hooked ... (N slots patched)`.

Only those two toasts are shown on screen (plus rare serious errors such as the web server stopping). Everything
else goes to the log file only.

## Web UI
* The header dot is green when a game is hooked. Each slot has a dropdown (load or remove a figure) and a **Save**
  button that writes the game's progress back to the dump file.
* Messages appear at the bottom for 4 seconds. **Queued** means the game had not picked the request up within
  2 seconds; it is applied the next time the game polls the portal and the slot is remembered.
* Errors show a code: `-1` the payload could not write or verify the request in the game's memory (look for
  `write:` lines in the log), `-3` an earlier request was still waiting so this one was not queued.
* Figure dumps live in `/data/skylanders/figures`; slot selections are kept in
  `/data/skylanders/skyportal_etahen_selection.txt` and cleared when the game exits.

## Saving progress
Figures keep their progress (XP, gold, upgrades, hats ...) inside the 1024-byte dump, and the game writes it back to
the emulated figure with the portal `W` command. The payload now copies that back to the figure file on its own:
* about 4 times a second it compares each loaded figure with the file; once the game has stopped writing (the copy is
  unchanged on two polls in a row) the file is rewritten atomically (`.tmp` + rename). The page shows
  `Progress saved: <file>`.
* changing or removing a slot saves the old figure first. **Save now** forces a save immediately.
* the first overwrite of a file copies the untouched original to `/data/skylanders/backup/<name>`.
* a figure whose file is loaded in two slots at once is written by whichever slot saved last; load each file once.
* `autosave=0` in `skyportal.ini` turns it off (only **Save now** writes). The game quitting before the next poll
  (under half a second) can lose that last write; quitting from the in-game menu first avoids it.

## Configuration (`/data/skylanders/skyportal.ini`)
| Key | Default | Meaning |
|---|---|---|
| `port=` | `8787` | web UI port |
| `settle_ms=` | `25000` | wait after the game's title appears before hooking. Hooking while the game is still starting up (before it asks for the portal) can leave a black screen, so keep this generous. If the game quits or relaunches during the wait, the attach is skipped |
| `idle_status=` | `22` | answer an idle IN poll with a status report about this often (ms of the game's idle sleeping); `0` = off, only replies and figure changes complete IN polls |
| `autosave=` | `1` | write game progress back to the figure files automatically; `0` = only the Save now button |
| `stage=` | off | diagnostic limit (1..4) for bring-up only; leave it out for a normal run |
| `title=CUSAxxxxx ctx=0 check=0` | see sample | one line per game; `ctx` = whether sceUsbdInit/GetDeviceList/HandleEvents* take a leading context argument |

The file on the console overrides the built-in defaults, so a stale `settle_ms=` or `stage=` line there wins over
anything in the source.

## Build
```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk      # example path
export ETAHEN_SDK=~/etaHEN-SDK                   # only for libhijacker (headers + libhijacker.a); build that SDK once first
cmake --preset ps5 && cmake --build --preset ps5     # -> build-ps5/bin/skyportal.elf
```
`prebuilt/hook_blob.h` and `prebuilt/web_ui.h` are committed; after changing `hook/`, `src/sky_portal.c`,
`include/sky_hooks.h` or `web/` run `make prebuilt`, then rebuild (`touch plugin/main_payload.cpp src/injector.c`
first if the ELF looks stale). Host checks: `make test`, `make demo && build/host_demo`, `tools/syntax_check_ps5.sh`.
Compiler warnings from the SDK and libhijacker sources are normal; look for `error:` and a final
`Linking CXX executable bin/skyportal.elf`. The log line `writing code (N bytes)` shows the size of the hook blob
that actually got injected, which tells you whether the ELF on the console is the build you expect.

## Diagnosing with the log
The log is **appended** across runs; delete it before a diagnostic run and read the last session. The hook records
a trace ring (first 96 events) and call counters:

| Trace | Meaning |
|---|---|
| `O` / `o` | command bytes the game sent (control `SET_REPORT` / interrupt OUT). Identical repeats are collapsed to 4 |
| `C` | other control requests |
| `I` / `G` | reply delivered to the game (interrupt IN / `GET_REPORT`) |
| `Y` | `AllocTransfer`: first two arguments and the pointer returned |
| `X` | `SubmitTransfer`: first two arguments, before any early return |
| `T` / `U` | raw bytes 0..31 / 32..63 of each submitted transfer struct |
| `B` | first bytes of that transfer's buffer |
| `P` | an IN transfer is waiting although a reply is expected |

## State damage check
The hook's writable state (about 26 KB) lives in a run of zero bytes found in the game's data segment, so memory that the game has simply
not used yet can look free. The state holds a magic number, three canaries and a 1 KB guard band that the hook never writes; the daemon
checks them once a second and logs `STATE DAMAGED by the game: mask 0x.. first at 0x..` (mask: 1 magic, 2/4/8 canaries, 16 guard band)
or `state at 0x.. is unreadable now`. If that line appears before a crash, the game is using the cave and it has to move.
The daemon copies the trace out every 50 ms and every log line starts with `[+seconds]` since the payload started, so a crash can be
lined up with what you did on screen (the game's memory is gone once it dies, so anything newer than the last copy is lost).
The trace keeps its first 32 events (handshake) and then rotates, so the last ~64 events before a crash are in the log.

## Troubleshooting
* **"Incompatible Portal of Power"**: the game never received a reply to its first command (`R`). Check the log:
  `O ... 52 00` repeating with no `I`/`G` line means replies are not reaching the game. The cause found on Trap Team
  was `AllocTransfer` returning NULL when its first argument was non-zero, so no transfer was ever usable; it now
  ignores that argument.
* **Black screen after relaunching**: the hook landed too early; raise `settle_ms`.
* **No "game hooked" toast**: read the last lines of the log; hook failures are only logged.
* **Figure missing from the web UI dropdown**: the figure list reply is limited to about 5 KB (roughly 180 names).

## Status
| Part | State |
|---|---|
| Portal protocol, hook blob, injector, API, daemon | tested on host (`make test`, ASan/UBSan) |
| `plugin/*`, `CMakeLists.txt` | builds with the PS5 toolchain; runs on a PS5 (firmware 13.42) with Trap Team |

## Known gaps
* Interrupt-OUT audio: after `M` turns the speaker on, interrupt-OUT packets are dropped if the game has never sent a
  command on that endpoint (it uses SET_REPORT), otherwise only packets starting with a known command letter are accepted.
  A game that streams audio *and* sends commands on OUT with audio starting with a command letter can still be misread.
  Look for `o` trace lines / `cmds_seen` if a game misbehaves while sound plays. `R` turns the speaker off again.
* Lights: `C r g b` and `L 0 r g b` set the ring colour, `L 1 level` / `L 3` the side-light brightness, `J 0 r g b ms_lo ms_hi`
  fades the ring. None of it is shown on the PS5; the last request is recorded and logged as `lights: ring=#rrggbb ...`.
  `J` is only honoured and acknowledged (one-byte `J` report) at exactly 7 bytes; `C` and `L` get no reply. Layouts are taken
  from the All-in-One Toypad Emulator, where `L`/`J` are also marked as assumed.
* A real portal returns a status report on every IN poll. The hook now does too, at a bounded rate (`idle_status=`, default 22 ms of the
  game's own idle sleeping in its USB event loop; only after the game has sent its first command). `idle_status=0` restores the old
  behaviour (IN polls complete only for replies and figure changes). Not yet tried on a console.
  reply or a figure change.
* Unverified: `sceUsbdCheckConnected` semantics, libusb struct layouts beyond what Trap Team exercises, PS4
  backward-compat processes under libhijacker for other titles.
* Keep a physical portal unplugged. Only the Swap Force and Trap Team import sets have been analysed.

## Licence note
libhijacker comes from the etaHEN SDK; check its licence before redistributing a built ELF.
