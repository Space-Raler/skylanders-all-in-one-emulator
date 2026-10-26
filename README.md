# Skylanders All-in-One Emulator — Development Foundation

This repository contains the shared software foundation for an all-in-one Skylanders portal emulator. **Trap Team on PS5 is the first test target**, not the only intended game. The longer-term goal is to reuse the portable portal core across multiple Skylanders titles and add game-specific compatibility where needed.

## Project status

This is an active development foundation, not a confirmed working PS5 portal emulator yet. Host-side tests exercise the portable core and adapter logic; they do not prove that a PS5 game can see a virtual USB portal. The PS5-side transport, USB device behavior, and in-game figure recognition remain unverified.

## What's included

- `build_v12/src/sky/core/sky_core.c` and `.h` — portable portal state and command handling.
- `build_v12/src/sky/core/sky_hid_adapter.*` — host-independent HID adapter contract.
- `build_v12/src/sky/core/sky_usb_control.*` — control-request dispatch/response layer.
- `build_v12/tests/` — host-side tests for the portable code.
- `build_v12/tools/` — development utilities for portal debugging and figure dumps.
- `build_v12/docs/` — source comparison, protocol notes, and PS5 transport feasibility research.
- `build_v12/PROJECT_CHECKLIST.txt` — current milestones, completed work, and outstanding tasks.
- `build_v12/README.md` — more detailed technical notes for the foundation.

## Run the host-side tests

On Linux with a C compiler and Make installed, extract this archive and run:

```sh
cd build_v12
make test
make sanitize
```

`make test` runs the C core tests and the project's test suite. `make sanitize` runs the sanitizer-enabled checks where supported by the compiler/runtime.

## Current development plan

1. Keep the shared portal protocol core separate from platform-specific code.
2. Test control-transfer responses separately from interrupt-IN reports.
3. Implement and validate a real PS5-side transport path for the target firmware.
4. Test whether Trap Team recognizes the portal and reads a legally obtained figure dump.
5. Expand compatibility to additional Skylanders games while retaining a shared core.

## Important limitations

- Passing host-side tests is not evidence of PS5 compatibility or in-game recognition.
- The current adapter/control dispatcher is not itself a complete USB device driver and does not make the PS5 USB host port act as a peripheral.
- Protocol differences between emulator references need to be resolved against actual target-game behavior where possible.
- Use figure dumps and other data that you are authorized to use.

See `build_v12/PROJECT_CHECKLIST.txt` and `build_v12/docs/` for detailed progress and open questions.
