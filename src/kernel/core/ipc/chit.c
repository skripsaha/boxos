
#include "chit.h"
#include "process.h"
#include "op_registry.h"
#include "pit.h"

void ChitInit(Chit *c)
{
    spinlock_init(&c->lock);
    memset(&c->view, 0, sizeof(c->view));
}

void ChitGive(const OpContext *ctx, const char *holder, uint64_t detail)
{
    if (ctx->async_owns_crates) *ctx->async_owns_crates = true;
    if (ctx->submit_cookie == 0 || !ctx->proc) return;

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