/*
 * Chit — implementation. See chit.h for what it is and what it is not.
 */

#include "chit.h"
#include "process.h"
#include "op_registry.h"
#include "pit.h"      /* pit_get_uptime_ms — the same clock Nightwatch judges by */

void ChitInit(Chit *c)
{
    spinlock_init(&c->lock);
    memset(&c->view, 0, sizeof(c->view));
}

void ChitGive(const OpContext *ctx, const char *holder, uint64_t detail)
{
    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
    if (ctx->submit_cookie == 0 || !ctx->proc) return;   /* nobody waits for this one */

    Chit    *c   = &ctx->proc->chit;
    uint64_t now = pit_get_uptime_ms();
    ChitView unpaid;
    bool     was_due = false;

    spin_lock(&c->lock);
    if (c->view.state == CHIT_DUE) {
        was_due = true;
        unpaid  = c->view;
    }
    c->view.cookie   = ctx->submit_cookie;
    c->view.state    = CHIT_PENDING;
    c->view.holder   = holder;
    c->view.detail   = detail;
    c->view.since_ms = now;
    spin_unlock(&c->lock);

    /* The strand asked for something new while the kernel still owed it the
     * last answer. Only a wait abandoned on a caller-named deadline can get
     * here, and the answer it walked away from was DETERMINED — the kernel had
     * it and did not deliver it in time, or did not deliver it at all. That
     * is the fact the overwrite would otherwise erase. */
    if (was_due)
        kprintf("[CHIT] DEFECT: pid %u moved on from token 0x%06x while %s still "
                "owed its answer — due %lu ms and never delivered\n",
                ctx->proc->pid, unpaid.cookie,
                unpaid.holder ? unpaid.holder : "?",
                (unsigned long)(now - unpaid.since_ms));
}

void ChitDue(process_t *p, uint32_t cookie)
{
    if (!p || cookie == 0) return;
    Chit *c = &p->chit;
    spin_lock(&c->lock);
    if (c->view.cookie == cookie && c->view.state == CHIT_PENDING) {
        c->view.state    = CHIT_DUE;
        c->view.since_ms = pit_get_uptime_ms();
    }
    spin_unlock(&c->lock);
}

void ChitKeep(process_t *p, uint32_t cookie)
{
    if (!p || cookie == 0) return;
    Chit *c = &p->chit;
    /* One load before the lock: an answer to a token this strand never had a
     * chit for (the ordinary synchronous reply) pays nothing more than this.
     * The lock re-checks it — the load is a filter, not the decision. */
    if (__atomic_load_n(&c->view.cookie, __ATOMIC_RELAXED) != cookie) return;
    spin_lock(&c->lock);
    if (c->view.cookie == cookie && c->view.state != CHIT_KEPT) {
        c->view.state    = CHIT_KEPT;
        c->view.since_ms = pit_get_uptime_ms();
    }
    spin_unlock(&c->lock);
}

void ChitPeek(process_t *p, ChitView *out)
{
    Chit *c = &p->chit;
    spin_lock(&c->lock);
    *out = c->view;
    spin_unlock(&c->lock);
}
