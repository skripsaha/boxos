
#include <memory_resource>
#include <new>

namespace std::pmr {

memory_resource::~memory_resource() = default;

namespace {

class NewDeleteResource final : public memory_resource {
    void *do_allocate(size_t bytes, size_t align) override
    {
        if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return ::operator new(bytes, align_val_t{align});
        return ::operator new(bytes);
    }
    void do_deallocate(void *p, size_t bytes, size_t align) override
    {
        if (align > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            ::operator delete(p, bytes, align_val_t{align});
        else
            ::operator delete(p, bytes);
    }
    bool do_is_equal(const memory_resource &o) const noexcept override
    {
        return this == &o;
    }
};

class NullResource final : public memory_resource {
    void *do_allocate(size_t, size_t) override { throw bad_alloc{}; }
    void  do_deallocate(void *, size_t, size_t) override {}
    bool  do_is_equal(const memory_resource &o) const noexcept override
    {
        return this == &o;
    }
};

memory_resource *g_default = nullptr;

}

memory_resource *new_delete_resource() noexcept
{
    static NewDeleteResource r;
    return &r;
}

memory_resource *null_memory_resource() noexcept
{
    static NullResource r;
    return &r;
}

memory_resource *get_default_resource() noexcept
{
    memory_resource *d = __atomic_load_n(&g_default, __ATOMIC_ACQUIRE);
    if (!d) {
        memory_resource *nd  = new_delete_resource();
        memory_resource *exp = nullptr;
        __atomic_compare_exchange_n(&g_default, &exp, nd, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        d = __atomic_load_n(&g_default, __ATOMIC_ACQUIRE);
    }
    return d;
}

memory_resource *set_default_resource(memory_resource *r) noexcept
{
    if (!r) r = new_delete_resource();
    memory_resource *prev = __atomic_exchange_n(&g_default, r, __ATOMIC_ACQ_REL);
    return prev ? prev : new_delete_resource();
}

}