#ifndef BOXCXX_BOX_REFLEX_H
#define BOXCXX_BOX_REFLEX_H

#include <utility>

#include "box/cxx/touch.h"
#include "box/cxx/manifest.h"
#include "box/touch.h"
#include "box/error.h"

namespace box {

class reflex {
    TouchTag          tag_ = TOUCH_TAG_INVALID;
    compiled_manifest body_;

public:
    reflex() noexcept = default;

    reflex(const tag &on, compiled_manifest &&program) noexcept
        : body_(std::move(program))
    {
        TouchTag id = on.id();
        if (body_ && id != TOUCH_TAG_INVALID &&
            touch_claim(id, TOUCH_REACT, body_.handle(), 0) == OK)
            tag_ = id;
    }

    reflex(const reflex &)            = delete;
    reflex &operator=(const reflex &) = delete;

    reflex(reflex &&o) noexcept : tag_(o.tag_), body_(std::move(o.body_))
    {
        o.tag_ = TOUCH_TAG_INVALID;
    }
    reflex &operator=(reflex &&o) noexcept
    {
        if (this != &o) {
            release();
            tag_   = o.tag_;
            body_  = std::move(o.body_);
            o.tag_ = TOUCH_TAG_INVALID;
        }
        return *this;
    }

    ~reflex() { release(); }

    explicit operator bool() const noexcept { return tag_ != TOUCH_TAG_INVALID; }
    TouchTag id() const noexcept { return tag_; }
    bool     bound() const noexcept { return tag_ != TOUCH_TAG_INVALID; }

    const compiled_manifest &manifest() const noexcept { return body_; }

private:
    void release() noexcept
    {
        if (tag_ != TOUCH_TAG_INVALID) {
            touch_release(tag_);
            tag_ = TOUCH_TAG_INVALID;
        }
    }
};

}

#endif