
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

}

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

}
}