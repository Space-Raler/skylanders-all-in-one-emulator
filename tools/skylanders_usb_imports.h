/* Generated from decrypted PS4 SELFs + etaHEN-SDK libSceUsbd stub.
 * NID->name verified: SHA-1(name + PS4 salt), first 8 bytes reversed, base64 ('/'->'-');
 * validated on 5 known SDK pairs and 22/22 observed USB NIDs.
 * Slot VAs are PLT/GOT slots (JMPREL r_offset) in the game's main executable. */
#pragma once
#include <stdint.h>

typedef struct { uint64_t slot_va; const char *nid; const char *name; } sky_usb_import_t;

/* Swap Force (CUSA00019 per eboot strings); entry 0x918650 */
static const sky_usb_import_t sky_swap_force_usb_imports[] = {
    { 0x1b643a0ULL, "TOhg7P6kTH4", "sceUsbdInit" },
    { 0x1b643b0ULL, "L0EHgZZNVas", "sceUsbdSubmitTransfer" },
    { 0x1b643c0ULL, "EkqGLxWC-S0", "sceUsbdHandleEvents" },
    { 0x1b643c8ULL, "RRKFcKQ1Ka4", "sceUsbdControlTransfer" },
    { 0x1b643d0ULL, "BKMEGvfCPyU", "sceUsbdAttachKernelDriver" },
    { 0x1b643e0ULL, "Fq6+0Fm55xU", "sceUsbdExit" },
    { 0x1b643e8ULL, "bhomgbiQgeo", "sceUsbdGetDeviceDescriptor" },
    { 0x1b643f0ULL, "Dkm5qe8j3XE", "sceUsbdGetConfigDescriptor" },
    { 0x1b643f8ULL, "RLf56F-WjKQ", "sceUsbdKernelDriverActive" },
    { 0x1b64400ULL, "Y5go+ha6eDs", "sceUsbdDetachKernelDriver" },
    { 0x1b64408ULL, "AE+mHBHneyk", "sceUsbdClaimInterface" },
    { 0x1b64410ULL, "0ktE1PhzGFU", "sceUsbdAllocTransfer" },
    { 0x1b64418ULL, "t3J5pXxhJlI", "sceUsbdFillInterruptTransfer" },
    { 0x1b64420ULL, "Hvd3S--n25w", "sceUsbdFreeConfigDescriptor" },
    { 0x1b64428ULL, "8qB9Ar4P5nc", "sceUsbdGetDeviceList" },
    { 0x1b64430ULL, "t7WE9mb1TB8", "sceUsbdGetBusNumber" },
    { 0x1b64438ULL, "GjlCrU4GcIY", "sceUsbdGetDeviceAddress" },
    { 0x1b64440ULL, "VJ6oMq-Di2U", "sceUsbdOpen" },
    { 0x1b64448ULL, "EQ6SCLMqzkM", "sceUsbdFreeDeviceList" },
    { 0x1b64450ULL, "MlW6deWfPp0", "sceUsbdCheckConnected" },
};
#define SKY_SWAP_FORCE_USB_IMPORT_COUNT 20

/* Trap Team (identified from eboot strings); entry 0x4049e0 */
static const sky_usb_import_t sky_trap_team_usb_imports[] = {
    { 0x10386a8ULL, "TOhg7P6kTH4", "sceUsbdInit" },
    { 0x10386b8ULL, "Fq6+0Fm55xU", "sceUsbdExit" },
    { 0x10386d0ULL, "MlW6deWfPp0", "sceUsbdCheckConnected" },
    { 0x10386d8ULL, "RLf56F-WjKQ", "sceUsbdKernelDriverActive" },
    { 0x10386e0ULL, "BKMEGvfCPyU", "sceUsbdAttachKernelDriver" },
    { 0x10386e8ULL, "-sgi7EeLSO8", "sceUsbdFreeTransfer" },
    { 0x10386f0ULL, "8qB9Ar4P5nc", "sceUsbdGetDeviceList" },
    { 0x10386f8ULL, "bhomgbiQgeo", "sceUsbdGetDeviceDescriptor" },
    { 0x1038700ULL, "t7WE9mb1TB8", "sceUsbdGetBusNumber" },
    { 0x1038708ULL, "GjlCrU4GcIY", "sceUsbdGetDeviceAddress" },
    { 0x1038710ULL, "VJ6oMq-Di2U", "sceUsbdOpen" },
    { 0x1038718ULL, "EQ6SCLMqzkM", "sceUsbdFreeDeviceList" },
    { 0x1038720ULL, "L0EHgZZNVas", "sceUsbdSubmitTransfer" },
    { 0x1038728ULL, "+wU6CGuZcWk", "sceUsbdHandleEventsTimeout" },
    { 0x1038730ULL, "Dkm5qe8j3XE", "sceUsbdGetConfigDescriptor" },
    { 0x1038738ULL, "Y5go+ha6eDs", "sceUsbdDetachKernelDriver" },
    { 0x1038740ULL, "AE+mHBHneyk", "sceUsbdClaimInterface" },
    { 0x1038748ULL, "0ktE1PhzGFU", "sceUsbdAllocTransfer" },
    { 0x1038750ULL, "t3J5pXxhJlI", "sceUsbdFillInterruptTransfer" },
    { 0x1038758ULL, "Hvd3S--n25w", "sceUsbdFreeConfigDescriptor" },
    { 0x1038760ULL, "RRKFcKQ1Ka4", "sceUsbdControlTransfer" },
};
#define SKY_TRAP_TEAM_USB_IMPORT_COUNT 21
