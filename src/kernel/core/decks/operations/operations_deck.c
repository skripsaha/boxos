#include "operations_deck.h"
#include "klib.h"

static int op_buf_move(Event* event);
static int op_buf_fill(Event* event);
static int op_buf_xor(Event* event);
static int op_buf_hash(Event* event);
static int op_buf_cmp(Event* event);
static int op_buf_find(Event* event);
static int op_buf_pack(Event* event);
static int op_buf_unpack(Event* event);
static int op_bit_swap(Event* event);
static int op_val_add(Event* event);

static inline uint32_t read_u32(const uint8_t* data, size_t offset) {
    return ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) |
           ((uint32_t)data[offset + 3]);
}

static inline void write_u32(uint8_t* data, size_t offset, uint32_t value) {
    data[offset] = (value >> 24) & 0xFF;
    data[offset + 1] = (value >> 16) & 0xFF;
    data[offset + 2] = (value >> 8) & 0xFF;
    data[offset + 3] = value & 0xFF;
}

int operations_deck_handler(Event* event) {
    if (!event) {
        return -1;
    }

    uint8_t opcode = event_get_opcode(event, event->current_prefix_idx);

    switch (opcode) {
    case OP_BUF_MOVE:
        return op_buf_move(event);

    case OP_BUF_FILL:
        return op_buf_fill(event);

    case OP_BUF_XOR:
        return op_buf_xor(event);

    case OP_BUF_HASH:
        return op_buf_hash(event);

    case OP_BUF_CMP:
        return op_buf_cmp(event);

    case OP_BUF_FIND:
        return op_buf_find(event);

    case OP_BUF_PACK:
        return op_buf_pack(event);

    case OP_BUF_UNPACK:
        return op_buf_unpack(event);

    case OP_BIT_SWAP:
        return op_bit_swap(event);

    case OP_VAL_ADD:
        return op_val_add(event);

    default:
        debug_printf("[OPS_DECK] Unknown opcode 0x%02x\n", opcode);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }
}

static int op_buf_move(Event* event) {
    uint32_t from_off = read_u32(event->data, 0);
    uint32_t to_off = read_u32(event->data, 4);
    uint32_t len = read_u32(event->data, 8);

    if (len > EVENT_DATA_SIZE ||
        from_off > EVENT_DATA_SIZE - len ||
        to_off > EVENT_DATA_SIZE - len) {
        debug_printf("[OPS_DECK] BUF_MOVE: out of bounds (from=%u to=%u len=%u)\n",
                from_off, to_off, len);
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    memmove(&event->data[to_off], &event->data[from_off], len);
    return 0;
}

static int op_buf_fill(Event* event) {
    uint32_t offset = read_u32(event->data, 0);
    uint32_t len = read_u32(event->data, 4);
    uint8_t byte = (uint8_t)(read_u32(event->data, 8) & 0xFF);

    if (len > EVENT_DATA_SIZE || offset > EVENT_DATA_SIZE - len) {
        debug_printf("[OPS_DECK] BUF_FILL: out of bounds\n");
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    memset(&event->data[offset], byte, len);
    return 0;
}

static int op_buf_xor(Event* event) {
    uint32_t offset = read_u32(event->data, 0);
    uint32_t len = read_u32(event->data, 4);
    uint8_t mask = (uint8_t)(read_u32(event->data, 8) & 0xFF);

    if (len > EVENT_DATA_SIZE || offset > EVENT_DATA_SIZE - len) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    for (uint32_t i = 0; i < len; i++) {
        event->data[offset + i] ^= mask;
    }

    return 0;
}

// ROL5 additive hash (NOT CRC32): for each byte, hash += byte; hash = ROL(hash, 5)
// Input: [0-3] offset, [4-7] len, [8-11] target_off for 32-bit result
static int op_buf_hash(Event* event) {
    uint32_t offset = read_u32(event->data, 0);
    uint32_t len = read_u32(event->data, 4);
    uint32_t target_off = read_u32(event->data, 8);

    if (len > EVENT_DATA_SIZE ||
        offset > EVENT_DATA_SIZE - len ||
        target_off > EVENT_DATA_SIZE - 4) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint32_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        hash += event->data[offset + i];
        hash = (hash << 5) | (hash >> 27);  // rotate left 5 bits
    }

    write_u32(event->data, target_off, hash);
    return 0;
}

static int op_buf_cmp(Event* event) {
    uint32_t off1 = read_u32(event->data, 0);
    uint32_t off2 = read_u32(event->data, 4);
    uint32_t len = read_u32(event->data, 8);

    if (len > EVENT_DATA_SIZE ||
        off1 > EVENT_DATA_SIZE - len ||
        off2 > EVENT_DATA_SIZE - len) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    int result = memcmp(&event->data[off1], &event->data[off2], len);
    write_u32(event->data, 0, (result == 0) ? 0 : 1);

    return 0;
}

static int op_buf_find(Event* event) {
    uint32_t pattern_len = read_u32(event->data, 0);

    if (pattern_len == 0 || pattern_len > EVENT_DATA_SIZE - 16) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint8_t* pattern = &event->data[4];
    uint32_t search_len = EVENT_DATA_SIZE - 4 - pattern_len;
    uint32_t found_offset = 0xFFFFFFFF;

    for (uint32_t i = 0; i < search_len; i++) {
        if (memcmp(&event->data[4 + pattern_len + i], pattern, pattern_len) == 0) {
            found_offset = 4 + pattern_len + i;
            break;
        }
    }

    write_u32(event->data, 0, found_offset);
    return 0;
}

// RLE compress: data[0..3]=src_offset, data[4..7]=src_len, data[8..11]=dst_offset
// Output at dst_offset: [count][byte] pairs. count=0 marks end.
// Result: data[0..3] = compressed size (0 on failure)
static int op_buf_pack(Event* event) {
    uint32_t src_off = read_u32(event->data, 0);
    uint32_t src_len = read_u32(event->data, 4);
    uint32_t dst_off = read_u32(event->data, 8);

    if (src_len == 0 || src_off > EVENT_DATA_SIZE - src_len ||
        dst_off >= EVENT_DATA_SIZE || src_off == dst_off) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    // Work on a temporary buffer to avoid src/dst overlap issues
    uint8_t tmp[EVENT_DATA_SIZE];
    uint32_t out = 0;
    uint32_t max_out = EVENT_DATA_SIZE - dst_off;

    uint32_t i = 0;
    while (i < src_len) {
        uint8_t byte = event->data[src_off + i];
        uint8_t count = 1;

        while (i + count < src_len && count < 255 &&
               event->data[src_off + i + count] == byte) {
            count++;
        }

        if (out + 2 > max_out) {
            // Not enough space for compressed output
            write_u32(event->data, 0, 0);
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        tmp[out++] = count;
        tmp[out++] = byte;
        i += count;
    }

    memcpy(&event->data[dst_off], tmp, out);
    write_u32(event->data, 0, out);
    return 0;
}

// RLE decompress: data[0..3]=src_offset, data[4..7]=src_len, data[8..11]=dst_offset
// Input at src_offset: [count][byte] pairs
// Result: data[0..3] = decompressed size (0 on failure)
static int op_buf_unpack(Event* event) {
    uint32_t src_off = read_u32(event->data, 0);
    uint32_t src_len = read_u32(event->data, 4);
    uint32_t dst_off = read_u32(event->data, 8);

    if (src_len == 0 || src_len % 2 != 0 ||
        src_off > EVENT_DATA_SIZE - src_len ||
        dst_off >= EVENT_DATA_SIZE || src_off == dst_off) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    uint8_t tmp[EVENT_DATA_SIZE];
    uint32_t out = 0;
    uint32_t max_out = EVENT_DATA_SIZE - dst_off;

    for (uint32_t i = 0; i + 1 < src_len; i += 2) {
        uint8_t count = event->data[src_off + i];
        uint8_t byte = event->data[src_off + i + 1];

        if (out + count > max_out) {
            write_u32(event->data, 0, 0);
            event->state = EVENT_STATE_ERROR;
            return -1;
        }

        memset(&tmp[out], byte, count);
        out += count;
    }

    memcpy(&event->data[dst_off], tmp, out);
    write_u32(event->data, 0, out);
    return 0;
}

static int op_bit_swap(Event* event) {
    uint32_t offset = read_u32(event->data, 0);
    uint32_t len = read_u32(event->data, 4);
    uint32_t mode = read_u32(event->data, 8);

    if (len > EVENT_DATA_SIZE || offset > EVENT_DATA_SIZE - len) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    switch (mode) {
    case 0:  // 16-bit swap
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint8_t tmp = event->data[offset + i];
            event->data[offset + i] = event->data[offset + i + 1];
            event->data[offset + i + 1] = tmp;
        }
        break;

    case 1:  // 32-bit swap
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            uint8_t tmp0 = event->data[offset + i];
            uint8_t tmp1 = event->data[offset + i + 1];
            event->data[offset + i] = event->data[offset + i + 3];
            event->data[offset + i + 1] = event->data[offset + i + 2];
            event->data[offset + i + 2] = tmp1;
            event->data[offset + i + 3] = tmp0;
        }
        break;

    case 2:  // 64-bit swap
        for (uint32_t i = 0; i + 7 < len; i += 8) {
            for (int j = 0; j < 4; j++) {
                uint8_t tmp = event->data[offset + i + j];
                event->data[offset + i + j] = event->data[offset + i + 7 - j];
                event->data[offset + i + 7 - j] = tmp;
            }
        }
        break;

    default:
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    return 0;
}

static int op_val_add(Event* event) {
    uint32_t offset = read_u32(event->data, 0);
    uint32_t type_size = read_u32(event->data, 4);
    int32_t delta = (int32_t)read_u32(event->data, 8);

    if (type_size > EVENT_DATA_SIZE || offset > EVENT_DATA_SIZE - type_size) {
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    switch (type_size) {
    case 1: {
        uint8_t val = event->data[offset];
        event->data[offset] = val + (int8_t)delta;
        break;
    }
    case 2: {
        uint16_t val = (event->data[offset] << 8) | event->data[offset + 1];
        val += (int16_t)delta;
        event->data[offset] = (val >> 8) & 0xFF;
        event->data[offset + 1] = val & 0xFF;
        break;
    }
    case 4: {
        uint32_t val = read_u32(event->data, offset);
        val += delta;
        write_u32(event->data, offset, val);
        break;
    }
    default:
        event->state = EVENT_STATE_ERROR;
        return -1;
    }

    return 0;
}
