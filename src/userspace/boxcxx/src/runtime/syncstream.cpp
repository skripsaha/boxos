/*
 * syncstream.cpp — the registry of emit() locks, one entry per wrapped
 * stream buffer.
 *
 * [syncstream.syncbuf.virtuals] lets emit() touch the wrapped buffer "while
 * holding a lock uniquely associated with *wrapped", and that word — unique —
 * is what this file exists for. A header cannot own it: every syncbuf naming
 * the same target must find the SAME lock, so there has to be exactly one
 * registry in the program, which is exactly one translation unit.
 *
 * What a target is: the sink a syncbuf writes through. A cabin has a handful
 * of them (the screen, a file, a string buffer under test), so the registry
 * is an intrusive list walked under one mutex — no table to size wrong, no
 * hash to collide, no ceiling on how many targets a cabin may have. An entry
 * appears when the first syncbuf names its target and disappears when the
 * last one lets go, so a program that opens and closes a thousand files in
 * sequence holds one entry at a time, not a thousand.
 *
 * The registry mutex is constant-initialised (std::mutex has a constexpr
 * default constructor over a two-word kernel word), so it is alive before any
 * dynamic initialiser runs and needs no init-order priority of its own.
 */

#include <__bits/syncbuf_base>

#include <mutex>

namespace std {
namespace __sync {

struct Target {
    const void *key;
    unsigned    refs;
    Target     *next;
    mutex       lock;
};

namespace {

mutex   g_registry;
Target *g_head = nullptr;

} // namespace

Target *Acquire(const void *wrapped)
{
    if (!wrapped) return nullptr;

    lock_guard<mutex> guard(g_registry);
    for (Target *t = g_head; t; t = t->next) {
        if (t->key == wrapped) {
            ++t->refs;
            return t;
        }
    }
    // Throwing here is what [syncstream.syncbuf.cons] allows a syncbuf
    // constructor to do ("Throws: nothing unless ... memory allocation"), and
    // it is the honest answer: a syncbuf that cannot find its lock cannot
    // promise the atomicity its whole contract is.
    Target *t = new Target{wrapped, 1, g_head, {}};
    g_head    = t;
    return t;
}

void Release(Target *t) noexcept
{
    if (!t) return;

    lock_guard<mutex> guard(g_registry);
    if (--t->refs != 0) return;
    for (Target **link = &g_head; *link; link = &(*link)->next) {
        if (*link == t) {
            *link = t->next;
            break;
        }
    }
    delete t;
}

void Lock(Target *t) noexcept
{
    if (t) t->lock.lock();
}

void Unlock(Target *t) noexcept
{
    if (t) t->lock.unlock();
}

} // namespace __sync
} // namespace std
