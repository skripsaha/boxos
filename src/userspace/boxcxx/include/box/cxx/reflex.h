// boxcxx — box::reflex  (the RAII face of the kernel's TOUCH_REACT mode)
//
// A reflex binds a box::compiled_manifest to a Touch tag and asks the kernel to
// RUN that manifest in-kernel on every touch of the tag (TOUCH_REACT). It is
// BoxOS's non-Unix replacement for an async-signal handler: the reaction is a
// declarative, kernel-executed Manifest — NOT a userspace upcall the kernel jumps
// into. A publisher touches the tag; touch_react_deliver → ManifestExecute runs
// the stored ops (forward to another tag, write a brook, flip a memtag, …) wholly
// in the kernel, with no signal frame, no re-entrancy on your stack, and none of
// the async-signal-safety minefield. box::subscription is the PULL side (the event
// is delivered to YOU); box::reflex is the PUSH side (the kernel acts for you).
//
//   box::manifest<> m;                              // forward the touch on to B
//   std::byte p[8]; /* [u16 full][u16 bare][u32 after_ms=0] — SYSTEM_OP_TOUCH_SEND */
//   m.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate, p);
//   box::compiled_manifest card(m);
//   box::reflex rx("sensor:spike"_tag, std::move(card));   // kernel now reacts
//   if (!rx) { /* compile- or claim-fail: inspect rx.manifest() to tell which */ }
//   ...                                             // every touch("sensor:spike")
//                                                   // runs `card` in the kernel
//   // rx leaves scope -> touch_release stops deliveries + drops the kernel's ref.
//
// Two independent refs on the compiled manifest: box::compiled_manifest holds the
// userspace handle ref (move-only); touch_claim(…, TOUCH_REACT, …) makes the kernel
// take its OWN ManifestRetain ref. The destructor BODY runs release() (touch_release)
// BEFORE the body_ member is destroyed, so deliveries stop and the kernel ref drops
// first, and only then does ~compiled_manifest drop our handle ref — the manifest can
// never be torn down while a delivery could still reach it.
//
// One claim per tag in a cabin: the kernel keeps a SINGLE Touch claim per tag for a
// cabin, so binding a reflex to a tag this cabin already holds a box::subscription on
// silently RECONFIGURES that one claim to REACT — the subscription's ring goes quiet,
// and whichever object is destroyed first releases the shared claim out from under the
// other. Pick one model per tag per cabin: the PULL box::subscription OR the PUSH
// box::reflex, never both on the same tag.
#ifndef BOXCXX_BOX_REFLEX_H
#define BOXCXX_BOX_REFLEX_H

#include <utility>

#include "box/cxx/touch.h"      // box::tag
#include "box/cxx/manifest.h"   // box::compiled_manifest
#include "box/touch.h"          // touch_claim / touch_release / TOUCH_REACT (C ABI)
#include "box/error.h"          // OK

namespace box {

class reflex {
    TouchTag          tag_ = TOUCH_TAG_INVALID;  // claimed id; INVALID once released
    compiled_manifest body_;                     // owns the userspace handle ref

public:
    reflex() noexcept = default;

    // Bind `program` to run in-kernel on every touch of `on` (TOUCH_REACT).
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

    // dtor BODY runs before body_'s dtor → touch_release precedes ~compiled_manifest.
    ~reflex() { release(); }

    explicit operator bool() const noexcept { return tag_ != TOUCH_TAG_INVALID; }
    TouchTag id() const noexcept { return tag_; }
    bool     bound() const noexcept { return tag_ != TOUCH_TAG_INVALID; }

    // Distinguishes compile-fail (no manifest) from claim-fail (manifest, no tag).
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

}  // namespace box

#endif  // BOXCXX_BOX_REFLEX_H
