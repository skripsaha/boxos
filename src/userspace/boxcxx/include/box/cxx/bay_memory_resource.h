// boxcxx — box::bay_memory_resource
//
// A std::pmr::memory_resource backed by a BoxOS Bay: a named, refcounted,
// cross-cabin shared region. Allocation is monotonic (bump) inside the Bay,
// so a producer cabin can build pmr containers straight into shared memory
// and any consumer cabin that bay_open()s the same tag sees the bytes.
// Deallocation is a no-op (monotonic); drop the whole resource to release
// the cabin's Bay claim. This is a box:: extension — not part of std.
#ifndef BOXCXX_BOX_BAY_MEMORY_RESOURCE_H
#define BOXCXX_BOX_BAY_MEMORY_RESOURCE_H

#include <memory_resource>
#include <new>

#include "box/bay.h"
#include "box/types.h"

namespace box {

class bay_memory_resource : public std::pmr::memory_resource {
    void  *base_;
    size_t size_;
    size_t off_;
    bool   owned_;

    static size_t AlignUp(size_t n, size_t a)
    {
        return (n + a - 1) & ~(a - 1);
    }

public:
    // Open (or create) the Bay named `tag`. With BAY_CREATE, `bytes` must be
    // non-zero; with BAY_OPEN it joins an existing Bay (size from the Bay).
    bay_memory_resource(const char *tag, uint64_t bytes,
                        uint32_t flags = BAY_CREATE)
        : base_(bay_open(tag, bytes, flags)),
          size_(base_ ? bay_size(base_) : 0), off_(0),
          owned_(base_ != nullptr)
    {
    }
    bay_memory_resource(const bay_memory_resource &) = delete;
    bay_memory_resource &operator=(const bay_memory_resource &) = delete;
    ~bay_memory_resource() override
    {
        if (owned_ && base_) bay_release(base_);
    }

    void  *data() const noexcept { return base_; }
    size_t size() const noexcept { return size_; }
    size_t used() const noexcept { return off_; }
    void   reset() noexcept { off_ = 0; }

protected:
    void *do_allocate(size_t bytes, size_t align) override
    {
        if (!base_) throw std::bad_alloc{};
        size_t cur     = reinterpret_cast<size_t>(base_) + off_;
        size_t aligned = AlignUp(cur, align);
        size_t pad     = aligned - cur;
        if (off_ + pad + bytes > size_) throw std::bad_alloc{};
        off_ += pad + bytes;
        return reinterpret_cast<void *>(aligned);
    }
    void do_deallocate(void *, size_t, size_t) override {} // monotonic
    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override
    {
        return this == &o;
    }
};

} // namespace box

#endif // BOXCXX_BOX_BAY_MEMORY_RESOURCE_H
