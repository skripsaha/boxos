#ifndef XHCI_INPUT_H
#define XHCI_INPUT_H

#include "ktypes.h"
#include "xhci.h"
#include "xhci_transfer.h"

// ============================================================================
// USB Input Device Types (Production Feature)
// ============================================================================

typedef enum {
    USB_INPUT_UNKNOWN = 0,
    USB_INPUT_KEYBOARD,
    USB_INPUT_MOUSE,
    USB_INPUT_TOUCHSCREEN,
    USB_INPUT_TABLET
} UsbInputType;

// ============================================================================
// USB HID Report Structures (USB HID Specification 1.11)
// ============================================================================

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keycodes[6];
} __attribute__((packed)) UsbKeyboardReport;

// Modifier bits
#define USB_KBD_MOD_LCTRL   (1 << 0)
#define USB_KBD_MOD_LSHIFT  (1 << 1)
#define USB_KBD_MOD_LALT    (1 << 2)
#define USB_KBD_MOD_LGUI    (1 << 3)
#define USB_KBD_MOD_RCTRL   (1 << 4)
#define USB_KBD_MOD_RSHIFT  (1 << 5)
#define USB_KBD_MOD_RALT    (1 << 6)
#define USB_KBD_MOD_RGUI    (1 << 7)

// USB HID Keycodes (subset)
#define USB_KBD_KEY_A       0x04
#define USB_KBD_KEY_B       0x05
#define USB_KBD_KEY_C       0x06
#define USB_KBD_KEY_ENTER   0x28
#define USB_KBD_KEY_ESCAPE  0x29
#define USB_KBD_KEY_BACKSPACE 0x2A
#define USB_KBD_KEY_TAB     0x2B
#define USB_KBD_KEY_SPACE   0x2C
#define USB_KBD_KEY_F1      0x3A
#define USB_KBD_KEY_F12     0x45

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) UsbMouseReport;

// Mouse button bits
#define USB_MOUSE_BTN_LEFT   (1 << 0)
#define USB_MOUSE_BTN_RIGHT  (1 << 1)
#define USB_MOUSE_BTN_MIDDLE (1 << 2)

// ============================================================================
// Callbacks and Events
// ============================================================================

typedef void (*UsbInputCallback)(UsbInputType Type, const void* Data, uint8_t Size);

typedef struct {
    uint32_t TotalDevices;
    uint32_t ConnectedDevices;
    uint32_t Keyboards;
    uint32_t Mice;
    uint32_t PollingInterval;
} UsbInputStats;

// ============================================================================
// Public API
// ============================================================================

// Initialization
void UsbInput_Init(void);
void UsbInput_Shutdown(void);
void UsbInput_Enable(bool Enable);

// Device management
int UsbInput_OnDeviceConnected(uint8_t SlotId, uint8_t Port, const void* ConfigData, uint16_t ConfigLen);
int UsbInput_OnDeviceDisconnected(uint8_t SlotId);

// Event registration
int UsbInput_RegisterKeyboard(UsbInputCallback Callback);
int UsbInput_RegisterMouse(UsbInputCallback Callback);

// Report processing
void UsbInput_ProcessKeyboardReport(const UsbKeyboardReport* Report);
void UsbInput_ProcessMouseReport(const UsbMouseReport* Report);

// Polling
int UsbInput_StartPolling(void);
void UsbInput_StopPolling(void);

// Statistics
void UsbInput_GetStats(UsbInputStats* Stats);

#endif // XHCI_INPUT_H
