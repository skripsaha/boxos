/*
 * cxxabi_typeinfo.cpp — Itanium type_info hierarchy: key functions
 * (vtables live here) and the [except.handle] catch-matching machinery.
 *
 * Identity is pointer equality: BoxOS binaries are single static images,
 * vague linkage merges duplicate typeinfo emissions.
 */

#include "cxxabi_typeinfo.h"

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace std {

type_info::~type_info() = default;

// Default: exact-type handlers only.
bool type_info::__do_catch(const type_info *thrown, void **, unsigned) const
{
    return this == thrown;
}

bool type_info::__do_upcast(const __cxxabiv1::__class_type_info *,
                            void **) const
{
    return false;
}

bad_cast::~bad_cast() = default;
const char *bad_cast::what() const noexcept { return "std::bad_cast"; }

bad_typeid::~bad_typeid() = default;
const char *bad_typeid::what() const noexcept { return "std::bad_typeid"; }

} // namespace std

namespace __cxxabiv1 {

// Compiler-emitted null checks around typeid(*p) / failed &-casts.
extern "C" [[noreturn]] void __cxa_bad_typeid()
{
    throw std::bad_typeid{};
}

extern "C" [[noreturn]] void __cxa_bad_cast()
{
    throw std::bad_cast{};
}

__fundamental_type_info::~__fundamental_type_info() = default;
__array_type_info::~__array_type_info() = default;
__function_type_info::~__function_type_info() = default;
__enum_type_info::~__enum_type_info() = default;
__class_type_info::~__class_type_info() = default;
__si_class_type_info::~__si_class_type_info() = default;
__vmi_class_type_info::~__vmi_class_type_info() = default;
__pbase_type_info::~__pbase_type_info() = default;
__pointer_type_info::~__pointer_type_info() = default;
__pointer_to_member_type_info::~__pointer_to_member_type_info() = default;

// ── class hierarchy upcast walk ─────────────────────────────────────────

namespace {

struct UpcastSearch {
    const __class_type_info *target;
    void *adjusted   = nullptr;
    int   path_count = 0;   // distinct public subobjects found
};

void WalkUpcast(const __class_type_info *node, void *object,
                UpcastSearch *search)
{
    if (node == search->target) {
        // Diamond virtual inheritance reaches the same subobject through
        // several paths — identical adjusted pointers count once.
        if (search->path_count == 0 || search->adjusted != object) {
            search->path_count++;
            search->adjusted = object;
        }
        return;
    }

    const std::type_info &kind = typeid(*node);

    if (kind == typeid(__si_class_type_info)) {
        // Single non-virtual public base at offset 0 by ABI definition.
        const auto *si = static_cast<const __si_class_type_info *>(node);
        WalkUpcast(si->__base_type, object, search);
        return;
    }

    if (kind == typeid(__vmi_class_type_info)) {
        const auto *vmi = static_cast<const __vmi_class_type_info *>(node);
        for (unsigned i = 0; i < vmi->__base_count; ++i) {
            const __base_class_type_info &base = vmi->__base_info[i];
            if (!base.IsPublic()) continue;

            long offset = base.Offset();
            void *base_obj;
            if (base.IsVirtual()) {
                // Virtual base: __offset is the vbase-offset slot in the
                // vtable; the displacement lives there.
                const char *vptr = *reinterpret_cast<const char *const *>(object);
                long displacement =
                    *reinterpret_cast<const long *>(vptr + offset);
                base_obj = static_cast<char *>(object) + displacement;
            } else {
                base_obj = static_cast<char *>(object) + offset;
            }
            WalkUpcast(base.__base_type, base_obj, search);
            if (search->path_count > 1) return;   // ambiguous — stop early
        }
    }
}

} // namespace

bool __class_type_info::__do_upcast(const __class_type_info *target,
                                    void **obj) const
{
    UpcastSearch search{target};
    WalkUpcast(this, *obj, &search);
    if (search.path_count != 1) return false;   // not found or ambiguous
    *obj = search.adjusted;
    return true;
}

bool __si_class_type_info::__do_upcast(const __class_type_info *target,
                                       void **obj) const
{
    return __class_type_info::__do_upcast(target, obj);
}

bool __vmi_class_type_info::__do_upcast(const __class_type_info *target,
                                        void **obj) const
{
    return __class_type_info::__do_upcast(target, obj);
}

bool __class_type_info::__do_catch(const std::type_info *thrown, void **obj,
                                   unsigned) const
{
    if (this == thrown) return true;
    // catch(Base) accepts a thrown Derived: walk the THROWN type's graph.
    return thrown->__do_upcast(this, obj);
}

// ── pointer catches ─────────────────────────────────────────────────────

namespace {

bool IsNullptrType(const std::type_info *ti)
{
    const char *n = ti->name();
    return n[0] == 'D' && n[1] == 'n' && n[2] == '\0';
}

bool IsVoidType(const std::type_info *ti)
{
    const char *n = ti->name();
    return n[0] == 'v' && n[1] == '\0';
}

} // namespace

bool __pointer_type_info::__do_catch(const std::type_info *thrown,
                                     void **obj, unsigned outer) const
{
    if (this == thrown) {
        if (outer < 2) *obj = *reinterpret_cast<void **>(*obj);
        return true;
    }

    // Thrown std::nullptr_t matches any pointer handler; the stored
    // representation (all-zero) reinterprets as a null T* directly.
    if (IsNullptrType(thrown)) {
        if (outer < 2) *obj = *reinterpret_cast<void **>(*obj);
        return true;
    }

    if (typeid(*thrown) != typeid(__pointer_type_info)) return false;
    const auto *thrown_ptr = static_cast<const __pointer_type_info *>(thrown);

    // cv rules: the handler's pointee must carry every qualifier of the
    // thrown pointee. Below the outermost level, qualifiers must allow
    // the standard qualification-conversion (track via `outer`).
    unsigned thrown_quals  = thrown_ptr->__flags &
                             (__const_mask | __volatile_mask);
    unsigned handler_quals = __flags & (__const_mask | __volatile_mask);
    if (thrown_quals & ~handler_quals) return false;
    if ((outer & 1) == 0 && thrown_quals != handler_quals) {
        // Past the first level a qualification difference requires every
        // outer level to be const — tracked bit lost → reject.
        return false;
    }
    unsigned next_outer = (handler_quals & __const_mask) ? (outer | 1) : 0;
    next_outer |= 2;   // mark "below outermost" for value vs object mode

    // At the outermost level the match yields the pointer VALUE
    // (adjustedPtr for pointer catches IS the pointer, per ABI).
    if (outer < 2) *obj = *reinterpret_cast<void **>(*obj);

    if (this->__pointee == thrown_ptr->__pointee) return true;

    // catch(void*) accepts any object pointer.
    if (IsVoidType(__pointee)) return true;

    // catch(Base*) accepts thrown Derived*: adjust the pointer value.
    // A null pointer matches without adjustment.
    auto IsClassKind = [](const std::type_info *ti) {
        const std::type_info &k = typeid(*ti);
        return k == typeid(__class_type_info) ||
               k == typeid(__si_class_type_info) ||
               k == typeid(__vmi_class_type_info);
    };
    if (IsClassKind(__pointee) && IsClassKind(thrown_ptr->__pointee)) {
        const auto *handler_cls =
            static_cast<const __class_type_info *>(__pointee);
        const auto *thrown_cls =
            static_cast<const __class_type_info *>(thrown_ptr->__pointee);
        if (*obj == nullptr) return true;
        return thrown_cls->__do_upcast(handler_cls, obj);
    }

    // Nested pointer levels (T** and deeper): recurse in object mode.
    if (typeid(*__pointee) == typeid(__pointer_type_info)) {
        const auto *handler_inner =
            static_cast<const __pointer_type_info *>(__pointee);
        return handler_inner->__do_catch(thrown_ptr->__pointee, obj,
                                         next_outer);
    }

    return false;
}

bool __pointer_to_member_type_info::__do_catch(const std::type_info *thrown,
                                               void **obj, unsigned) const
{
    if (this == thrown) return true;
    if (IsNullptrType(thrown)) return true;
    (void)obj;
    // Contravariant member-pointer conversions in catch clauses are
    // intentionally out of scope (documented in CONFORMANCE).
    return false;
}

// ── personality entry point ─────────────────────────────────────────────

bool CatchMatches(const std::type_info *catch_type,
                  const std::type_info *throw_type, void **thrown_object)
{
    if (!catch_type) return true;   // catch(...)
    return catch_type->__do_catch(throw_type, thrown_object, 1);
}

} // namespace __cxxabiv1
