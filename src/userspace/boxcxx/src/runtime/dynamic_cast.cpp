
#include "cxxabi_typeinfo.h"
#include <cstddef>

namespace __cxxabiv1 {

namespace {

const void *BaseObject(const void *obj, const __base_class_type_info &base)
{
    long offset = base.Offset();
    if (base.IsVirtual()) {
        const char *vptr = *reinterpret_cast<const char *const *>(obj);
        offset = *reinterpret_cast<const long *>(vptr + offset);
    }
    return static_cast<const char *>(obj) + offset;
}

bool FindSubobject(const __class_type_info *node, const void *obj,
                   const __class_type_info *want, const void *want_addr,
                   bool public_only)
{
    if (node == want && obj == want_addr) return true;

    const std::type_info &kind = typeid(*node);

    if (kind == typeid(__si_class_type_info)) {
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

struct DstHits {
    const __class_type_info *src_type;
    const void *src_ptr;
    const __class_type_info *dst_type;

    const void *addr[2]   = {nullptr, nullptr};
    bool leg_public[2]   = {false, false};
    int  count            = 0;

    void Add(const void *a, bool pub)
    {
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
        if (FindSubobject(node, obj, hits->src_type, hits->src_ptr, true))
            hits->Add(obj, true);
        else if (FindSubobject(node, obj, hits->src_type, hits->src_ptr,
                               false))
            hits->Add(obj, false);
        return;
    }
    if (hits->count > 1) return;

    const std::type_info &kind = typeid(*node);

    if (kind == typeid(__si_class_type_info)) {
        const auto *si = static_cast<const __si_class_type_info *>(node);
        CollectDst(si->__base_type, obj, hits);
        return;
    }

    if (kind == typeid(__vmi_class_type_info)) {
        const auto *vmi = static_cast<const __vmi_class_type_info *>(node);
        for (unsigned i = 0; i < vmi->__base_count; ++i) {
            const __base_class_type_info &base = vmi->__base_info[i];
            CollectDst(base.__base_type, BaseObject(obj, base), hits);
            if (hits->count > 1) return;
        }
    }
}

}

extern "C" void *__dynamic_cast(const void *src_ptr,
                                const __class_type_info *src_type,
                                const __class_type_info *dst_type,
                                ptrdiff_t src2dst_offset)
{
    if (src_ptr == nullptr) return nullptr;

    const char *vtable = *reinterpret_cast<const char *const *>(src_ptr);
    ptrdiff_t offset_to_top = reinterpret_cast<const ptrdiff_t *>(vtable)[-2];
    const auto *whole_type = static_cast<const __class_type_info *>(
        reinterpret_cast<const std::type_info *const *>(vtable)[-1]);
    const void *whole = static_cast<const char *>(src_ptr) + offset_to_top;

    if (whole_type == dst_type) {
        if (src2dst_offset >= 0 &&
            static_cast<const char *>(src_ptr) - src2dst_offset == whole)
            return const_cast<void *>(whole);
        if (FindSubobject(whole_type, whole, src_type, src_ptr, true))
            return const_cast<void *>(whole);
        return nullptr;
    }

    if (src2dst_offset != -2) {
        DstHits hits{src_type, src_ptr, dst_type};
        CollectDst(whole_type, whole, &hits);
        if (hits.count == 1 && hits.leg_public[0])
            return const_cast<void *>(hits.addr[0]);
    }

    if (!FindSubobject(whole_type, whole, src_type, src_ptr, true))
        return nullptr;
    void *adjusted = const_cast<void *>(whole);
    if (whole_type->__do_upcast(dst_type, &adjusted))
        return adjusted;
    return nullptr;
}

}