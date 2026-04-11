#include "xhci_input.h"
#include "xhci_hid.h"
#include "xhci_transfer.h"
#include "xhci_enumeration.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "atomics.h"

// ============================================================================
// USB Input Device Management (Production Feature)
// ============================================================================

#define USB_INPUT_MAX_DEVICES    8
#define USB_INPUT_POLL_INTERVAL  10  // ms

typedef struct {
    uint8_t slot_id;
    uint8_t port;
    UsbInputType type;
    bool is_connected;
    bool is_configured;
    uint8_t interface_num;
    uint8_t endpoint_addr;
    uint8_t endpoint_dci;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t config_value;
    uint64_t transfer_buffer_phys;
    void* transfer_buffer;
    void* transfer;  // xhci_transfer_t* - forward declared
    uint32_t last_poll_time;
} UsbInputDevice;

static UsbInputDevice g_UsbInputDevices[USB_INPUT_MAX_DEVICES];
static spinlock_t g_UsbInputLock;
static uint8_t g_UsbInputDeviceCount = 0;
static bool g_UsbInputEnabled = false;

// Callbacks for input events
static UsbInputCallback g_KeyboardCallback = NULL;
static UsbInputCallback g_MouseCallback = NULL;

// ============================================================================
// Internal Helpers
// ============================================================================

static UsbInputDevice* UsbInputAllocSlot(void) {
    spin_lock(&g_UsbInputLock);
    
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        if (!g_UsbInputDevices[i].is_connected) {
            g_UsbInputDevices[i].is_connected = true;
            g_UsbInputDeviceCount++;
            spin_unlock(&g_UsbInputLock);
            return &g_UsbInputDevices[i];
        }
    }
    
    spin_unlock(&g_UsbInputLock);
    return NULL;
}

static void UsbInputFreeSlot(UsbInputDevice* Device) {
    if (!Device) return;
    
    spin_lock(&g_UsbInputLock);
    Device->is_connected = false;
    Device->is_configured = false;
    g_UsbInputDeviceCount--;
    spin_unlock(&g_UsbInputLock);
}

static UsbInputDevice* UsbInputFindBySlot(uint8_t SlotId) {
    spin_lock(&g_UsbInputLock);
    
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        if (g_UsbInputDevices[i].is_connected && 
            g_UsbInputDevices[i].slot_id == SlotId) {
            spin_unlock(&g_UsbInputLock);
            return &g_UsbInputDevices[i];
        }
    }
    
    spin_unlock(&g_UsbInputLock);
    return NULL;
}

// ============================================================================
// Keyboard Support
// ============================================================================

int UsbInput_RegisterKeyboard(UsbInputCallback Callback) {
    spin_lock(&g_UsbInputLock);
    g_KeyboardCallback = Callback;
    spin_unlock(&g_UsbInputLock);
    return 0;
}

void UsbInput_ProcessKeyboardReport(const UsbKeyboardReport* Report) {
    if (!Report || !g_UsbInputEnabled) return;
    
    // Forward to callback if registered
    if (g_KeyboardCallback) {
        g_KeyboardCallback(USB_INPUT_KEYBOARD, Report, sizeof(UsbKeyboardReport));
    }
    
    // Also process through legacy keyboard handler
    xhci_process_keyboard_report((usb_boot_keyboard_report_t*)Report);
}

// ============================================================================
// Mouse Support
// ============================================================================

int UsbInput_RegisterMouse(UsbInputCallback Callback) {
    spin_lock(&g_UsbInputLock);
    g_MouseCallback = Callback;
    spin_unlock(&g_UsbInputLock);
    return 0;
}

void UsbInput_ProcessMouseReport(const UsbMouseReport* Report) {
    if (!Report || !g_UsbInputEnabled) return;
    
    // Forward to callback if registered
    if (g_MouseCallback) {
        g_MouseCallback(USB_INPUT_MOUSE, Report, sizeof(UsbMouseReport));
    }
    
    debug_printf("[USB Mouse] Buttons=0x%02X DX=%d DY=%d\n",
                 Report->buttons, (int)Report->x, (int)Report->y);
}

// ============================================================================
// Device Enumeration and Configuration
// ============================================================================

int UsbInput_OnDeviceConnected(uint8_t SlotId, uint8_t Port, const void* ConfigData, uint16_t ConfigLen) {
    if (!ConfigData || ConfigLen < 9) return -1;
    
    // Allocate slot
    UsbInputDevice* Device = UsbInputAllocSlot();
    if (!Device) {
        debug_printf("[USB Input] No free input device slots\n");
        return -1;
    }
    
    Device->slot_id = SlotId;
    Device->port = Port;
    Device->is_configured = false;
    Device->transfer = NULL;
    
    // Parse configuration descriptor
    usb_keyboard_info_t KbInfo;
    int Result = xhci_parse_config_descriptor((void*)ConfigData, ConfigLen, &KbInfo);
    
    if (Result == 0) {
        // Keyboard detected
        Device->type = USB_INPUT_KEYBOARD;
        Device->interface_num = KbInfo.interface_num;
        Device->endpoint_addr = KbInfo.endpoint_addr;
        Device->endpoint_dci = KbInfo.endpoint_dci;
        Device->max_packet_size = KbInfo.max_packet_size;
        Device->interval = KbInfo.interval;
        Device->config_value = KbInfo.config_value;
        
        debug_printf("[USB Input] Keyboard detected: slot=%u port=%u endpoint=0x%02X\n",
                     SlotId, Port, Device->endpoint_addr);
    } else {
        // Check for mouse (simplified detection)
        // In production, would parse HID descriptor for mouse
        Device->type = USB_INPUT_UNKNOWN;
        debug_printf("[USB Input] Unknown HID device: slot=%u port=%u\n", SlotId, Port);
        UsbInputFreeSlot(Device);
        return -1;
    }
    
    // Allocate transfer buffer
    Device->transfer_buffer = pmm_alloc_zero(1);
    if (!Device->transfer_buffer) {
        debug_printf("[USB Input] Failed to allocate transfer buffer\n");
        UsbInputFreeSlot(Device);
        return -1;
    }
    
    Device->transfer_buffer_phys = (uint64_t)vmm_virt_to_phys_direct(Device->transfer_buffer);
    
    Device->is_configured = true;
    return 0;
}

int UsbInput_OnDeviceDisconnected(uint8_t SlotId) {
    UsbInputDevice* Device = UsbInputFindBySlot(SlotId);
    if (!Device) return -1;
    
    debug_printf("[USB Input] Device disconnected: slot=%u type=%u\n",
                 SlotId, Device->type);
    
    // Cancel any pending transfers
    if (Device->transfer) {
        // Transfer cancellation would go here
        Device->transfer = NULL;
    }
    
    // Free transfer buffer
    if (Device->transfer_buffer) {
        pmm_free(Device->transfer_buffer, 1);
        Device->transfer_buffer = NULL;
    }
    
    UsbInputFreeSlot(Device);
    return 0;
}

// ============================================================================
// Polling and Interrupt Handling
// ============================================================================

// Forward declaration - full implementation when transfer API is ready
// static void UsbInput_TransferComplete(xhci_transfer_t* Transfer, void* Context);

int UsbInput_StartPolling(void) {
    if (!g_UsbInputEnabled) return -1;
    
    spin_lock(&g_UsbInputLock);
    
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        UsbInputDevice* Device = &g_UsbInputDevices[i];
        if (!Device->is_connected || !Device->is_configured) continue;
        
        // Submit interrupt transfer
        // In production, would use proper xHCI interrupt transfer API
        debug_printf("[USB Input] Starting poll for device %u\n", i);
    }
    
    spin_unlock(&g_UsbInputLock);
    return 0;
}

void UsbInput_StopPolling(void) {
    spin_lock(&g_UsbInputLock);
    
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        UsbInputDevice* Device = &g_UsbInputDevices[i];
        if (Device->transfer) {
            // Cancel transfer
            Device->transfer = NULL;
        }
    }
    
    spin_unlock(&g_UsbInputLock);
}

// ============================================================================
// Initialization and Shutdown
// ============================================================================

void UsbInput_Init(void) {
    memset(g_UsbInputDevices, 0, sizeof(g_UsbInputDevices));
    g_UsbInputDeviceCount = 0;
    g_UsbInputEnabled = false;
    g_KeyboardCallback = NULL;
    g_MouseCallback = NULL;
    spinlock_init(&g_UsbInputLock);
    
    debug_printf("[USB Input] Initialized (max=%d devices)\n", USB_INPUT_MAX_DEVICES);
}

void UsbInput_Shutdown(void) {
    UsbInput_StopPolling();
    
    spin_lock(&g_UsbInputLock);
    g_UsbInputEnabled = false;
    
    // Free all device resources
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        UsbInputDevice* Device = &g_UsbInputDevices[i];
        if (Device->is_connected) {
            if (Device->transfer_buffer) {
                pmm_free(Device->transfer_buffer, 1);
                Device->transfer_buffer = NULL;
            }
        }
    }
    
    memset(g_UsbInputDevices, 0, sizeof(g_UsbInputDevices));
    g_UsbInputDeviceCount = 0;
    spin_unlock(&g_UsbInputLock);
    
    debug_printf("[USB Input] Shutdown complete\n");
}

void UsbInput_Enable(bool Enable) {
    spin_lock(&g_UsbInputLock);
    g_UsbInputEnabled = Enable;
    spin_unlock(&g_UsbInputLock);
    
    if (Enable) {
        UsbInput_StartPolling();
    } else {
        UsbInput_StopPolling();
    }
}

// ============================================================================
// Statistics and Monitoring
// ============================================================================

void UsbInput_GetStats(UsbInputStats* Stats) {
    if (!Stats) return;
    
    memset(Stats, 0, sizeof(UsbInputStats));
    
    spin_lock(&g_UsbInputLock);
    
    Stats->TotalDevices = USB_INPUT_MAX_DEVICES;
    Stats->ConnectedDevices = g_UsbInputDeviceCount;
    Stats->Keyboards = 0;
    Stats->Mice = 0;
    
    for (uint8_t i = 0; i < USB_INPUT_MAX_DEVICES; i++) {
        if (g_UsbInputDevices[i].is_connected) {
            if (g_UsbInputDevices[i].type == USB_INPUT_KEYBOARD) {
                Stats->Keyboards++;
            } else if (g_UsbInputDevices[i].type == USB_INPUT_MOUSE) {
                Stats->Mice++;
            }
        }
    }
    
    spin_unlock(&g_UsbInputLock);
}
