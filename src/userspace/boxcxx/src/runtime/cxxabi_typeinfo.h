#ifndef BOXCXX_CXXABI_TYPEINFO_H
#define BOXCXX_CXXABI_TYPEINFO_H

#include <typeinfo>
#include <cstdint>

namespace __cxxabiv1 {

class __fundamental_type_info : public std::type_info {
public:
    ~__fundamental_type_info() override;
};

class __array_type_info : public std::type_info {
public:
    ~__array_type_info() override;
};

class __function_type_info : public std::type_info {
public:
    ~__function_type_info() override;
};

class __enum_type_info : public std::type_info {
public:
    ~__enum_type_info() override;
};

class __class_type_info : public std::type_info {
public:
    ~__class_type_info() override;

    bool __do_catch(const std::type_info *thrown, void **obj,
                    unsigned outer) const override;
    bool __do_upcast(const __class_type_info *target,
                     void **obj) const override;
};

class __si_class_type_info : public __class_type_info {
public:
    const __class_type_info *__base_type;

    ~__si_class_type_info() override;
    bool __do_upcast(const __class_type_info *target,
                     void **obj) const override;
};

struct __base_class_type_info {
    const __class_type_info *__base_type;
    long __offset_flags;

    enum __offset_flags_masks {
        __virtual_mask = 0x1,
        __public_mask  = 0x2,
        __offset_shift = 8,
    };

    bool IsPublic() const { return __offset_flags & __public_mask; }
    bool IsVirtual() const { return __offset_flags & __virtual_mask; }
    long Offset() const { return __offset_flags >> __offset_shift; }
};

class __vmi_class_type_info : public __class_type_info {
public:
    unsigned int __flags;
    unsigned int __base_count;
    __base_class_type_info __base_info[1];

    enum __flags_masks {
        __non_diamond_repeat_mask = 0x1,
        __diamond_shaped_mask     = 0x2,
    };

    ~__vmi_class_type_info() override;
    bool __do_upcast(const __class_type_info *target,
                     void **obj) const override;
};

class __pbase_type_info : public std::type_info {
public:
    unsigned int __flags;
    const std::type_info *__pointee;

    enum __masks {
        __const_mask            = 0x1,
        __volatile_mask         = 0x2,
        __restrict_mask         = 0x4,
        __incomplete_mask       = 0x8,
        __incomplete_class_mask = 0x10,
        __transaction_safe_mask = 0x20,
        __noexcept_mask         = 0x40,
    };

    ~__pbase_type_info() override;
};

class __pointer_type_info : public __pbase_type_info {
public:
    ~__pointer_type_info() override;
    bool __do_catch(const std::type_info *thrown, void **obj,
                    unsigned outer) const override;
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    const __class_type_info *__context;

    ~__pointer_to_member_type_info() override;
    bool __do_catch(const std::type_info *thrown, void **obj,
                    unsigned outer) const override;
};

bool CatchMatches(const std::type_info *catch_type,
                  const std::type_info *throw_type, void **thrown_object);

}

namespace abi = __cxxabiv1;

#endif