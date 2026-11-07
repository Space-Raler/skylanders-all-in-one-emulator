/* Struct layouts the games see through the sceUsbd* API.
 *
 * ASSUMPTION (UNVERIFIED): sceUsbd is a libusb-1.0 derivative and these structs
 * keep libusb's x86-64 layout. Every size/offset the hook relies on is pinned
 * with _Static_assert so a wrong guess shows up at build time, but the *guess*
 * itself must be confirmed against the libSceUsbd header in your SDK (or a
 * disassembly of how the games read/write these fields).
 */
#ifndef SKY_SCEUSBD_ABI_H
#define SKY_SCEUSBD_ABI_H
#include <stddef.h>
#include <stdint.h>

#define LIBUSB_TRANSFER_TYPE_CONTROL   0
#define LIBUSB_TRANSFER_TYPE_BULK      2
#define LIBUSB_TRANSFER_TYPE_INTERRUPT 3

#define LIBUSB_TRANSFER_COMPLETED 0
#define LIBUSB_ERROR_INVALID_PARAM (-2)
#define LIBUSB_ERROR_NO_DEVICE     (-4)
#define LIBUSB_ERROR_NOT_FOUND     (-5)
#define LIBUSB_ERROR_PIPE          (-9)
#define LIBUSB_ERROR_NO_MEM        (-11)

struct sky_usb_device_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

struct sky_usb_endpoint_descriptor {
    uint8_t  bLength, bDescriptorType, bEndpointAddress, bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval, bRefresh, bSynchAddress;
    const uint8_t *extra;
    int      extra_length;
};

struct sky_usb_interface_descriptor {
    uint8_t  bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t  bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol;
    uint8_t  iInterface;
    const struct sky_usb_endpoint_descriptor *endpoint;
    const uint8_t *extra;
    int      extra_length;
};

struct sky_usb_interface {
    const struct sky_usb_interface_descriptor *altsetting;
    int num_altsetting;
};

struct sky_usb_config_descriptor {
    uint8_t  bLength, bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces, bConfigurationValue, iConfiguration, bmAttributes, MaxPower;
    const struct sky_usb_interface *interface;
    const uint8_t *extra;
    int      extra_length;
};

struct sky_usb_transfer;
typedef void (*sky_usb_transfer_cb)(struct sky_usb_transfer *);

struct sky_usb_transfer {
    void          *dev_handle;
    uint8_t        flags;
    uint8_t        endpoint;
    uint8_t        type;
    unsigned int   timeout;
    int            status;
    int            length;
    int            actual_length;
    sky_usb_transfer_cb callback;
    void          *user_data;
    uint8_t       *buffer;
    int            num_iso_packets;
    /* iso_packet_desc[] follows in libusb; unused for a HID portal */
};

_Static_assert(sizeof(struct sky_usb_device_descriptor) == 18, "device descriptor size");
_Static_assert(offsetof(struct sky_usb_device_descriptor, idVendor) == 8, "idVendor offset");
_Static_assert(sizeof(struct sky_usb_endpoint_descriptor) == 32, "endpoint desc size (libusb x86-64)");
_Static_assert(sizeof(struct sky_usb_interface_descriptor) == 40, "interface desc size (libusb x86-64)");
_Static_assert(sizeof(struct sky_usb_config_descriptor) == 40, "config desc size (libusb x86-64)");
_Static_assert(offsetof(struct sky_usb_transfer, endpoint) == 9, "transfer.endpoint");
_Static_assert(offsetof(struct sky_usb_transfer, timeout) == 12, "transfer.timeout");
_Static_assert(offsetof(struct sky_usb_transfer, status) == 16, "transfer.status");
_Static_assert(offsetof(struct sky_usb_transfer, length) == 20, "transfer.length");
_Static_assert(offsetof(struct sky_usb_transfer, actual_length) == 24, "transfer.actual_length");
_Static_assert(offsetof(struct sky_usb_transfer, callback) == 32, "transfer.callback");
_Static_assert(offsetof(struct sky_usb_transfer, user_data) == 40, "transfer.user_data");
_Static_assert(offsetof(struct sky_usb_transfer, buffer) == 48, "transfer.buffer");
_Static_assert(offsetof(struct sky_usb_transfer, num_iso_packets) == 56, "transfer.num_iso_packets");

#endif
