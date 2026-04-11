#include "operations_deck.h"
#include "klib.h"
#include "vmm.h"
#include "process.h"
#include "../deck_utils.h"

static int op_buf_move(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_fill(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_xor(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_hash(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_cmp(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_find(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_pack(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_buf_unpack(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_bit_swap(Pocket* pocket, uint8_t* data, uint32_t data_len);
static int op_val_add(Pocket* pocket, uint8_t* data, uint32_t data_len);

int operations_deck_handler(Pocket* pocket, process_t* proc) {
    if (!pocket || !proc) {
        return -1;
    }

    uint8_t* data = NULL;
    if (pocket->data_length > 0) {
        data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
        if (!data) {
            pocket->error_code = ERR_INVALID_ADDRESS;
            return -1;
        }
    }

    uint32_t data_len = pocket->data_length;
    uint8_t opcode = pocket_get_opcode(pocket, pocket->current_prefix_idx);

    switch (opcode) {
    case OP_BUF_MOVE:
        return op_buf_move(pocket, data, data_len);

    case OP_BUF_FILL:
        return op_buf_fill(pocket, data, data_len);

    case OP_BUF_XOR:
        return op_buf_xor(pocket, data, data_len);

    case OP_BUF_HASH:
        return op_buf_hash(pocket, data, data_len);

    case OP_BUF_CMP:
        return op_buf_cmp(pocket, data, data_len);

    case OP_BUF_FIND:
        return op_buf_find(pocket, data, data_len);

    case OP_BUF_PACK:
        return op_buf_pack(pocket, data, data_len);

    case OP_BUF_UNPACK:
        return op_buf_unpack(pocket, data, data_len);

    case OP_BIT_SWAP:
        return op_bit_swap(pocket, data, data_len);

    case OP_VAL_ADD:
        return op_val_add(pocket, data, data_len);

    default:
        debug_printf("[OPS_DECK] Unknown opcode 0x%02x\n", opcode);
        pocket->error_code = ERR_INVALID_OPCODE;
        return -1;
    }
}

static int op_buf_move(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t from_off = deck_read_u32(&data[0]);
    uint32_t to_off   = deck_read_u32(&data[4]);
    uint32_t len      = deck_read_u32(&data[8]);

    if (len > data_len ||
        from_off > data_len - len ||
        to_off > data_len - len) {
        debug_printf("[OPS_DECK] BUF_MOVE: out of bounds (from=%u to=%u len=%u)\n",
                from_off, to_off, len);
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    memmove(&data[to_off], &data[from_off], len);
    return 0;
}

static int op_buf_fill(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t offset = deck_read_u32(&data[0]);
    uint32_t len    = deck_read_u32(&data[4]);
    uint8_t  byte   = (uint8_t)(deck_read_u32(&data[8]) & 0xFF);

    if (len > data_len || offset > data_len - len) {
        debug_printf("[OPS_DECK] BUF_FILL: out of bounds\n");
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    memset(&data[offset], byte, len);
    return 0;
}

static int op_buf_xor(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t offset = deck_read_u32(&data[0]);
    uint32_t len    = deck_read_u32(&data[4]);
    uint8_t  mask   = (uint8_t)(deck_read_u32(&data[8]) & 0xFF);

    if (len > data_len || offset > data_len - len) {
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    for (uint32_t i = 0; i < len; i++) {
        data[offset + i] ^= mask;
    }

    return 0;
}

// ROL5 additive hash (NOT CRC32): for each byte, hash += byte; hash = ROL(hash, 5)
// Input: [0-3] offset, [4-7] len, [8-11] target_off for 32-bit result
static int op_buf_hash(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t offset     = deck_read_u32(&data[0]);
    uint32_t len        = deck_read_u32(&data[4]);
    uint32_t target_off = deck_read_u32(&data[8]);

    if (len > data_len ||
        offset > data_len - len ||
        target_off > data_len - 4) {
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    uint32_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        hash += data[offset + i];
        hash = (hash << 5) | (hash >> 27);  // rotate left 5 bits
    }

    deck_write_u32(&data[target_off], hash);
    return 0;
}

static int op_buf_cmp(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t off1 = deck_read_u32(&data[0]);
    uint32_t off2 = deck_read_u32(&data[4]);
    uint32_t len  = deck_read_u32(&data[8]);

    if (len > data_len ||
        off1 > data_len - len ||
        off2 > data_len - len) {
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    int result = memcmp(&data[off1], &data[off2], len);
    deck_write_u32(&data[0], (result == 0) ? 0 : 1);

    return 0;
}

static int op_buf_find(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 8) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t pattern_len = deck_read_u32(&data[0]);

    if (pattern_len == 0 || pattern_len > data_len - 8) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint8_t* pattern    = &data[4];
    uint32_t search_len = data_len - 4 - pattern_len;
    uint32_t found_offset = 0xFFFFFFFF;

    for (uint32_t i = 0; i < search_len; i++) {
        if (memcmp(&data[4 + pattern_len + i], pattern, pattern_len) == 0) {
            found_offset = 4 + pattern_len + i;
            break;
        }
    }

    deck_write_u32(&data[0], found_offset);
    return 0;
}

// RLE compress: data[0..3]=src_offset, data[4..7]=src_len, data[8..11]=dst_offset
// Output at dst_offset: [count][byte] pairs. count=0 marks end.
// Result: data[0..3] = compressed size (0 on failure)
static int op_buf_pack(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t src_off = deck_read_u32(&data[0]);
    uint32_t src_len = deck_read_u32(&data[4]);
    uint32_t dst_off = deck_read_u32(&data[8]);

    if (src_len == 0 || src_off > data_len - src_len ||
        dst_off >= data_len || src_off == dst_off) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint8_t* tmp = vmalloc(data_len);
    if (!tmp) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    uint32_t out     = 0;
    uint32_t max_out = data_len - dst_off;
    uint32_t i       = 0;

    while (i < src_len) {
        uint8_t byte  = data[src_off + i];
        uint8_t count = 1;

        while (i + count < src_len && count < 255 &&
               data[src_off + i + count] == byte) {
            count++;
        }

        if (out + 2 > max_out) {
            deck_write_u32(&data[0], 0);
            pocket->error_code = ERR_BUFFER_TOO_SMALL;
            vfree(tmp);
            return -1;
        }

        tmp[out++] = count;
        tmp[out++] = byte;
        i += count;
    }

    memcpy(&data[dst_off], tmp, out);
    deck_write_u32(&data[0], out);
    vfree(tmp);
    return 0;
}

// RLE decompress: data[0..3]=src_offset, data[4..7]=src_len, data[8..11]=dst_offset
// Input at src_offset: [count][byte] pairs
// Result: data[0..3] = decompressed size (0 on failure)
static int op_buf_unpack(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t src_off = deck_read_u32(&data[0]);
    uint32_t src_len = deck_read_u32(&data[4]);
    uint32_t dst_off = deck_read_u32(&data[8]);

    if (src_len == 0 || src_len % 2 != 0 ||
        src_off > data_len - src_len ||
        dst_off >= data_len || src_off == dst_off) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint8_t* tmp = vmalloc(data_len);
    if (!tmp) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    uint32_t out     = 0;
    uint32_t max_out = data_len - dst_off;

    for (uint32_t i = 0; i + 1 < src_len; i += 2) {
        uint8_t count = data[src_off + i];
        uint8_t byte  = data[src_off + i + 1];

        if (out + count > max_out) {
            deck_write_u32(&data[0], 0);
            pocket->error_code = ERR_BUFFER_TOO_SMALL;
            vfree(tmp);
            return -1;
        }

        memset(&tmp[out], byte, count);
        out += count;
    }

    memcpy(&data[dst_off], tmp, out);
    deck_write_u32(&data[0], out);
    vfree(tmp);
    return 0;
}

static int op_bit_swap(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t offset = deck_read_u32(&data[0]);
    uint32_t len    = deck_read_u32(&data[4]);
    uint32_t mode   = deck_read_u32(&data[8]);

    if (len > data_len || offset > data_len - len) {
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    switch (mode) {
    case 0:  // 16-bit swap
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint8_t tmp = data[offset + i];
            data[offset + i]     = data[offset + i + 1];
            data[offset + i + 1] = tmp;
        }
        break;

    case 1:  // 32-bit swap
        for (uint32_t i = 0; i + 3 < len; i += 4) {
            uint8_t tmp0 = data[offset + i];
            uint8_t tmp1 = data[offset + i + 1];
            data[offset + i]     = data[offset + i + 3];
            data[offset + i + 1] = data[offset + i + 2];
            data[offset + i + 2] = tmp1;
            data[offset + i + 3] = tmp0;
        }
        break;

    case 2:  // 64-bit swap
        for (uint32_t i = 0; i + 7 < len; i += 8) {
            for (int j = 0; j < 4; j++) {
                uint8_t tmp = data[offset + i + j];
                data[offset + i + j]     = data[offset + i + 7 - j];
                data[offset + i + 7 - j] = tmp;
            }
        }
        break;

    default:
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    return 0;
}

static int op_val_add(Pocket* pocket, uint8_t* data, uint32_t data_len) {
    if (!data || data_len < 12) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t offset    = deck_read_u32(&data[0]);
    uint32_t type_size = deck_read_u32(&data[4]);
    int32_t  delta     = (int32_t)deck_read_u32(&data[8]);

    if (type_size > data_len || offset > data_len - type_size) {
        pocket->error_code = ERR_OUT_OF_RANGE;
        return -1;
    }

    switch (type_size) {
    case 1: {
        uint8_t val = data[offset];
        data[offset] = val + (int8_t)delta;
        break;
    }
    case 2: {
        uint16_t val = deck_read_u16(&data[offset]);
        val += (int16_t)delta;
        deck_write_u16(&data[offset], val);
        break;
    }
    case 4: {
        uint32_t val = deck_read_u32(&data[offset]);
        val += (uint32_t)delta;
        deck_write_u32(&data[offset], val);
        break;
    }
    default:
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    return 0;
}
