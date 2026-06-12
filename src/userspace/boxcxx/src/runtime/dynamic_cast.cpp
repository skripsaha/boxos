/*
 * dynamic_cast.cpp — Itanium C++ ABI §2.9.7 __dynamic_cast.
 *
 * Runtime check per [expr.dynamic.cast]/10 against the compiler-emitted
 * __class_type_info graphs (cxxabi_typeinfo.h shapes):
 *   10.1 down-cast : the src subobject is a public base of exactly one
 *                    dst object inside the most derived object.
 *   10.2 cross-cast: the src subobject is a public base subobject of the
 *                    most derived object, whose type has dst as an
 *                    unambiguous public base.
 *
 * Subobject identity is (type_info pointer, address) — vague linkage
 * merges duplicate typeinfo emissions in BoxOS's single static images,
 * and repeated non-virtual bases differ by address.
 */

#include "cxxabi_typeinfo.h"
#include <cstddef>

namespace __cxxabiv1 {

namespace {

const void *BaseObject(const void *obj, const __base_class_type_info &base)
{
    long offset = base.Offset();
    if (base.IsVirtual()) {
        // Virtual base: Offset() is the vbase-offset slot position in the
        // vtable; the actual displacement lives there.
        const char *vptr = *reinterpret_cast<const char *const *>(obj);
        offset = *reinterpret_cast<const long *>(vptr + offset);
    }
    return static_cast<const char *>(obj) + offset;
}

// Does the subobject (want, want_addr) appear under (node, obj)?
// public_only restricts the walk to all-public inheritance paths.
bool FindSubobject(const __class_type_info *node, const void *obj,
                   const __class_type_info *want, const void *want_addr,
                   bool public_only)
{
    if (node == want && obj == want_addr) return true;

    const std::type_info &kind = typeid(*node);

    if (kind == typeid(__si_class_type_info)) {
        // Single public non-virtual base at offset 0 by ABI definition.
        const auto *si = static_cast<const __si_class_type_info *>(node);
        return FindSubobject(si->__base_type, obj, want, want_addr,
                             public_only);
    }

    if (kind == typeid(__vmi_class_type_info)) {
        const auto *vmi = static_cast<const __vmi_class_type_info *>(node);
        for (unsigned i = 0; i < vmi->__base_count; ++i) {
            const __base_class_type_info &base = vmi->__base_info[i];
            if (public_only && !base.IsPublic()) continue;
            if (FindSubobject(base.__base_type, BaseObject(obj, base),
                              want, want_addr, public_only))
                return true;
        }
    }
    return false;
}

// Distinct dst subobjects of the whole that contain the src subobject.
// Only the 0/1/>1 distinction matters; a shared virtual src base under
// several dst objects is the way count exceeds 1.
struct DstHits {
    const __class_type_info *src_type;
    const void *src_ptr;
    const __class_type_info *dst_type;

    const void *addr[2]   = {nullptr, nullptr};
    bool leg_public[2]   = {false, false};   // public dst→src path exists
    int  count            = 0;

    void Add(const void *a, bool pub)
    {
        // A virtual dst base reaches the same subobject via several
        // paths — identical addresses merge.
        for (int i = 0; i < count && i < 2; ++i) {
            if (addr[i] == a) {
                leg_public[i] = leg_public[i] || pub;
                return;
            }
        }
        if (count < 2) {
            addr[count]        = a;
            leg_public[count] = pub;
        }
        count++;
    }
};

void CollectDst(const __class_type_info *node, const void *obj,
                DstHits *hits)
{
    if (node == hits->dst_type) {
        // A class is never its own base — no dst nodes deeper down.
        if (FindSubobject(node, obj, hits->src_type, hits->src_ptr, true))
            hits->Add(obj, true);
        else if (FindSubobject(node, obj, hits->src_type, hits->src_ptr,
                               false))
            hits->Add(obj, false);
        return;
    }
    if (hits->count > 1) return;   // clause 10.1 already dead

    const std::type_info &kind = typeid(*node);

    if (kind == typeid(__si_class_type_info)) {
        const auto *si = static_cast<const __si_class_type_info *>(node);
        CollectDst(si->__base_type, obj, hits);
        return;
    }

    if (kind == typeid(__vmi_class_type_info)) {
        const auto *vmi = static_cast<const __vmi_class_type_info *>(node);
        for (unsigned i = 0; i < vmi->__base_count; ++i) {
            // 10.1 puts no access requirement on the whole→dst leg, so
            // private arms are walked too.
            const __base_class_type_info &base = vmi->__base_info[i];
            CollectDst(base.__base_type, BaseObject(obj, base), hits);
            if (hits->count > 1) return;
        }
    }
}

} // namespace

extern "C" void *__dynamic_cast(const void *src_ptr,
                                const __class_type_info *src_type,
                                const __class_type_info *dst_type,
                                ptrdiff_t src2dst_offset)
{
    if (src_ptr == nullptr) return nullptr;

    // Vtable prefix (ABI §2.5.2): [-2] offset-to-top, [-1] type_info*.
    const char *vtable = *reinterpret_cast<const char *const *>(src_ptr);
    ptrdiff_t offset_to_top = reinterpret_cast<const ptrdiff_t *>(vtable)[-2];
    const auto *whole_type = static_cast<const __class_type_info *>(
        reinterpret_cast<const std::type_info *const *>(vtable)[-1]);
    const void *whole = static_cast<const char *>(src_ptr) + offset_to_top;

    if (whole_type == dst_type) {
        // Hint ≥ 0: src is dst's unique public non-virtual base at that
        // offset — a position match proves the cast without a walk. A
        // miss proves nothing (src_ptr may be a deeper src subobject).
        if (src2dst_offset >= 0 &&
            static_cast<const char *>(src_ptr) - src2dst_offset == whole)
            return const_cast<void *>(whole);
        if (FindSubobject(whole_type, whole, src_type, src_ptr, true))
            return const_cast<void *>(whole);
        return nullptr;
    }

    // 10.1 down-cast. Hint -2 = src is never a public base of dst, the
    // clause cannot hold — skip straight to the cross-cast.
    if (src2dst_offset != -2) {
        DstHits hits{src_type, src_ptr, dst_type};
        CollectDst(whole_type, whole, &hits);
        if (hits.count == 1 && hits.leg_public[0])
            return const_cast<void *>(hits.addr[0]);
    }

    // 10.2 cross-cast.
    if (!FindSubobject(whole_type, whole, src_type, src_ptr, true))
        return nullptr;
    void *adjusted = const_cast<void *>(whole);
    if (whole_type->__do_upcast(dst_type, &adjusted))
        return adjusted;
    return nullptr;
}

} // namespace __cxxabiv1
