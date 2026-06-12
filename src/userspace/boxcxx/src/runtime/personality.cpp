/*
 * personality.cpp — __gxx_personality_v0: the Itanium Level 2 C++
 * personality over the boxcxx unwinder. Decodes the LSDA
 * (.gcc_except_table): call-site table → action chain → type table.
 */

#include "cxxabi_typeinfo.h"

#include <unwind.h>
#include <cstdint>
#include <cstddef>

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace __cxxabiv1 {
struct CxaException;
}

namespace {

// Mirrors the layout in cxa_exception.cpp (single source of truth there;
// only the fields the personality touches are accessed via this view).
struct CxaExceptionView {
    size_t          referenceCount;
    std::type_info *exceptionType;
    void          (*exceptionDestructor)(void *);
    void           *unexpectedHandler;
    void           *terminateHandler;
    void           *nextException;
    int             handlerCount;
    int             handlerSwitchValue;
    const uint8_t  *actionRecord;
    const uint8_t  *languageSpecificData;
    void           *catchTemp;
    void           *adjustedPtr;
    _Unwind_Exception unwindHeader;
};

CxaExceptionView *ViewOf(_Unwind_Exception *exc)
{
    return reinterpret_cast<CxaExceptionView *>(
               reinterpret_cast<char *>(exc + 1)) -
           1;
}

uint64_t ReadUleb(const uint8_t **p)
{
    uint64_t r = 0;
    int      s = 0;
    uint8_t  b;
    do {
        b = *(*p)++;
        r |= uint64_t(b & 0x7F) << s;
        s += 7;
    } while (b & 0x80);
    return r;
}

int64_t ReadSleb(const uint8_t **p)
{
    int64_t r = 0;
    int     s = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        r |= int64_t(b & 0x7F) << s;
        s += 7;
    } while (b & 0x80);
    if (s < 64 && (b & 0x40)) r |= -(int64_t(1) << s);
    return r;
}

constexpr uint8_t kPeOmit = 0xFF;

uint64_t ReadEncoded(const uint8_t **p, uint8_t enc)
{
    if (enc == kPeOmit) return 0;
    const uint8_t *field = *p;
    uint64_t v;
    switch (enc & 0x0F) {
    case 0x00: { uint64_t t; __builtin_memcpy(&t, *p, 8); *p += 8; v = t; break; }
    case 0x01: v = ReadUleb(p); break;
    case 0x02: { uint16_t t; __builtin_memcpy(&t, *p, 2); *p += 2; v = t; break; }
    case 0x03: { uint32_t t; __builtin_memcpy(&t, *p, 4); *p += 4; v = t; break; }
    case 0x04: { uint64_t t; __builtin_memcpy(&t, *p, 8); *p += 8; v = t; break; }
    case 0x09: v = uint64_t(ReadSleb(p)); break;
    case 0x0B: { int32_t t; __builtin_memcpy(&t, *p, 4); *p += 4; v = uint64_t(int64_t(t)); break; }
    case 0x0C: { int64_t t; __builtin_memcpy(&t, *p, 8); *p += 8; v = uint64_t(t); break; }
    default: boxcxx::Panic("lsda: unsupported DW_EH_PE format");
    }
    if ((enc & 0x70) == 0x10) v += uint64_t(uintptr_t(field));   // pcrel
    else if ((enc & 0x70) != 0) boxcxx::Panic("lsda: unsupported application");
    if (enc & 0x80) v = *reinterpret_cast<const uint64_t *>(v);
    return v;
}

size_t EncodedSize(uint8_t enc)
{
    switch (enc & 0x0F) {
    case 0x00: case 0x04: case 0x0C: return 8;
    case 0x03: case 0x0B: return 4;
    case 0x02: case 0x0A: return 2;
    default: boxcxx::Panic("lsda: variable-size ttype encoding");
    }
}

const std::type_info *TTypeEntry(const uint8_t *ttype_base, uint8_t enc,
                                 int64_t index)
{
    const uint8_t *p = ttype_base - index * int64_t(EncodedSize(enc));
    uint64_t v = ReadEncoded(&p, enc);
    return reinterpret_cast<const std::type_info *>(v);
}

struct LsdaScan {
    bool           found_cleanup = false;
    bool           found_handler = false;
    uint64_t       landing_pad   = 0;
    int64_t        switch_value  = 0;   // filter for handlers, 0 cleanup
    void          *adjusted      = nullptr;
    const uint8_t *action_record = nullptr;
};

// Scans the LSDA of the frame at `ip` for the throw of `throw_type`.
bool ScanLsda(const uint8_t *lsda, uint64_t func_start, uint64_t ip,
              const std::type_info *throw_type, void *thrown_object,
              bool native, LsdaScan *out)
{
    const uint8_t *p = lsda;

    uint8_t  lpstart_enc = *p++;
    uint64_t lpstart =
        (lpstart_enc == kPeOmit) ? func_start : ReadEncoded(&p, lpstart_enc);

    uint8_t        ttype_enc  = *p++;
    const uint8_t *ttype_base = nullptr;
    if (ttype_enc != kPeOmit) {
        uint64_t off = ReadUleb(&p);
        ttype_base   = p + off;
    }

    uint8_t  cs_enc = *p++;
    uint64_t cs_len = ReadUleb(&p);
    const uint8_t *cs_end     = p + cs_len;
    const uint8_t *action_tab = cs_end;

    uint64_t key = ip - 1 - func_start;

    while (p < cs_end) {
        uint64_t start  = ReadEncoded(&p, cs_enc);
        uint64_t len    = ReadEncoded(&p, cs_enc);
        uint64_t pad    = ReadEncoded(&p, cs_enc);
        uint64_t action = ReadUleb(&p);

        if (key < start) break;          // table is sorted; we passed it
        if (key >= start + len) continue;

        if (pad == 0) return false;      // covered, but nothing to do

        out->landing_pad = lpstart + pad;

        if (action == 0) {               // cleanup only
            out->found_cleanup = true;
            out->switch_value  = 0;
            return true;
        }

        const uint8_t *act = action_tab + (action - 1);
        for (;;) {
            const uint8_t *record = act;
            int64_t filter = ReadSleb(&act);
            int64_t next   = ReadSleb(&act);

            if (filter == 0) {
                out->found_cleanup = true;
                out->switch_value  = 0;
            } else if (filter > 0) {
                if (!ttype_base)
                    boxcxx::Panic("lsda: catch filter without type table");
                const std::type_info *catch_type =
                    TTypeEntry(ttype_base, ttype_enc, filter);
                void *adjusted = thrown_object;
                bool matches =
                    catch_type == nullptr
                        ? true   // catch(...)
                        : (native && __cxxabiv1::CatchMatches(
                                         catch_type, throw_type, &adjusted));
                if (matches) {
                    out->found_handler = true;
                    out->switch_value  = filter;
                    out->adjusted      = catch_type ? adjusted
                                                    : thrown_object;
                    out->action_record = record;
                    return true;
                }
            } else {
                // filter < 0: exception specification (noexcept).
                // Anything reaching it violates the spec → handler.
                out->found_handler = true;
                out->switch_value  = filter;
                out->adjusted      = thrown_object;
                out->action_record = record;
                return true;
            }

            if (next == 0) break;
            // `next` is a self-relative displacement from the address of
            // the next-offset field itself.
            const uint8_t *next_field = record;
            (void)ReadSleb(&next_field);         // skip filter
            act = next_field + next;
        }
        return out->found_cleanup;
    }
    return false;
}

} // namespace

extern "C" _Unwind_Reason_Code
__gxx_personality_v0(int version, _Unwind_Action actions,
                     _Unwind_Exception_Class exception_class,
                     _Unwind_Exception *exc, _Unwind_Context *ctx)
{
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;

    const uint8_t *lsda = reinterpret_cast<const uint8_t *>(
        _Unwind_GetLanguageSpecificData(ctx));
    if (!lsda) return _URC_CONTINUE_UNWIND;

    bool native = (exception_class & ~0xFFull) ==
                  ((uint64_t('B') << 56) | (uint64_t('O') << 48) |
                   (uint64_t('X') << 40) | (uint64_t('S') << 32) |
                   (uint64_t('C') << 24) | (uint64_t('+') << 16) |
                   (uint64_t('+') << 8));

    CxaExceptionView *view = native ? ViewOf(exc) : nullptr;
    const std::type_info *throw_type = view ? view->exceptionType : nullptr;
    void *thrown_object = view ? static_cast<void *>(view + 1) : nullptr;

    LsdaScan scan;
    if (!ScanLsda(lsda, _Unwind_GetRegionStart(ctx), _Unwind_GetIP(ctx),
                  throw_type, thrown_object, native, &scan))
        return _URC_CONTINUE_UNWIND;

    if (actions & _UA_SEARCH_PHASE) {
        if (!scan.found_handler) return _URC_CONTINUE_UNWIND;
        if (view) {
            view->handlerSwitchValue = int(scan.switch_value);
            view->adjustedPtr        = scan.adjusted;
            view->catchTemp =
                reinterpret_cast<void *>(uintptr_t(scan.landing_pad));
            view->languageSpecificData = lsda;
            view->actionRecord         = scan.action_record;
        }
        return _URC_HANDLER_FOUND;
    }

    // Cleanup phase.
    bool handler_frame = (actions & _UA_HANDLER_FRAME) != 0;
    if (!handler_frame && !scan.found_cleanup && !scan.found_handler)
        return _URC_CONTINUE_UNWIND;
    if (!handler_frame && !scan.found_cleanup)
        return _URC_CONTINUE_UNWIND;   // handler match belongs to phase 1

    int64_t switch_value;
    uint64_t landing_pad;
    void *adjusted;
    if (handler_frame) {
        switch_value = scan.found_handler ? scan.switch_value : 0;
        landing_pad  = scan.landing_pad;
        adjusted     = scan.adjusted;
        if (view) view->adjustedPtr = adjusted;
    } else {
        switch_value = 0;
        landing_pad  = scan.landing_pad;
    }

    _Unwind_SetGR(ctx, 0, reinterpret_cast<_Unwind_Word>(exc));   // rax
    _Unwind_SetGR(ctx, 1, _Unwind_Word(switch_value));            // rdx
    _Unwind_SetIP(ctx, landing_pad);
    return _URC_INSTALL_CONTEXT;
}
