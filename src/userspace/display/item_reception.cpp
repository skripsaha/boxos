/*
 * item_reception.cpp — an area's ring of changes read, step by whole step,
 * and its events told back. See item_reception.h.
 */

#include "item_reception.h"

#include <cstring>

namespace items {

namespace {

enum class Found { Whole, Partial, Broken };

/* The record at At of a ring the writer has written up to Write: its head,
 * its length, and whether it is all there. */
Found RecordAt(std::span<const std::uint8_t> Ring, std::uint64_t At, std::uint64_t Write, ItemRecord &Head,
               std::uint32_t &Length) noexcept
{
    const auto Bytes = static_cast<std::uint32_t>(Ring.size());
    if (Write - At < sizeof(ItemRecord)) return Found::Partial;
    const auto Offset = static_cast<std::uint32_t>(At % Bytes);
    if (Bytes - Offset < sizeof(ItemRecord)) return Found::Broken;
    std::memcpy(&Head, Ring.data() + Offset, sizeof(Head));
    if (Head.Op == ITEM_OP_PAD && Head.Words == 0) {
        Length = Bytes - Offset;
    } else {
        if (Head.Words == 0) return Found::Broken;
        Length = static_cast<std::uint32_t>(Head.Words) * 8u;
    }
    if (Length > Bytes - Offset) return Found::Broken;
    if (Length > Write - At) return Found::Partial;
    return Found::Whole;
}

/* One record applied to Items: OK, or why not. */
error_t Apply(const ItemRecord &Head, std::span<const std::uint8_t> Body, Tree &Items, Reading &Got,
              const Listener &Tell)
{
    switch (Head.Op) {
    case ITEM_OP_PAD:
        return OK;
    case ITEM_OP_CREATE: {
        ItemCreate How{};
        if (Body.size() < sizeof(How)) return ERR_INVALID_ARGUMENT;
        std::memcpy(&How, Body.data(), sizeof(How));
        return Items.Create(Head.Item, How);
    }
    case ITEM_OP_REMOVE:
        return Items.Remove(Head.Item);
    case ITEM_OP_RESTACK: {
        ItemPlaceAmong How{};
        if (Body.size() < sizeof(How)) return ERR_INVALID_ARGUMENT;
        std::memcpy(&How, Body.data(), sizeof(How));
        return Items.Restack(Head.Item, How);
    }
    case ITEM_OP_CHANGE:
    case ITEM_OP_RESET: {
        ItemChange What{};
        if (Body.size() < sizeof(What)) return ERR_INVALID_ARGUMENT;
        std::memcpy(&What, Body.data(), sizeof(What));
        if (Head.Op == ITEM_OP_RESET) return Items.Reset(Head.Item, What.Property);
        if (What.Bytes > Body.size() - sizeof(What)) return ERR_INVALID_ARGUMENT;
        return Items.Change(Head.Item, What.Property, Body.subspan(sizeof(What), What.Bytes));
    }
    case ITEM_OP_BLOCK: {
        ItemBlockChanged Where{};
        if (Body.size() < sizeof(Where)) return ERR_INVALID_ARGUMENT;
        std::memcpy(&Where, Body.data(), sizeof(Where));
        if (Tell.BlockChanged) Tell.BlockChanged(Head.Item, Where);
        return OK;
    }
    case ITEM_OP_STEP: {
        ItemStep Step{};
        if (Body.size() < sizeof(Step)) return ERR_INVALID_ARGUMENT;
        std::memcpy(&Step, Body.data(), sizeof(Step));
        Got.Step    = Step.Number;
        Got.Stepped = true;
        return OK;
    }
    default:
        return ERR_UNSUPPORTED;
    }
}

}  // namespace

Reading ReadSteps(ItemAreaHeader &Head, std::span<const std::uint8_t> Ring, Tree &Items, const Listener &Tell)
{
    Reading Got;
    const auto          Bytes = static_cast<std::uint32_t>(Ring.size());
    const std::uint64_t Write = __atomic_load_n(&Head.Write, __ATOMIC_ACQUIRE);
    const std::uint64_t Read  = Head.Read;
    if (Bytes == 0 || Bytes % 8u || Write < Read || Write - Read > Bytes || Read % 8u || Write % 8u) {
        Got.Broken = true;
        return Got;
    }

    /* Where the last whole step ends: nothing past it is applied yet. */
    std::uint64_t At = Read, Whole = Read;
    while (At < Write) {
        ItemRecord    Record{};
        std::uint32_t Length = 0;
        const Found   Seen   = RecordAt(Ring, At, Write, Record, Length);
        if (Seen == Found::Broken) {
            Got.Broken = true;
            return Got;
        }
        if (Seen == Found::Partial) break;
        At += Length;
        if (Record.Op == ITEM_OP_STEP) Whole = At;
    }

    for (At = Read; At < Whole;) {
        ItemRecord    Record{};
        std::uint32_t Length = 0;
        if (RecordAt(Ring, At, Write, Record, Length) != Found::Whole) {
            Got.Broken = true;
            break;
        }
        const auto                    Offset = static_cast<std::uint32_t>(At % Bytes);
        const std::span<const std::uint8_t> Body =
            Record.Op == ITEM_OP_PAD ? std::span<const std::uint8_t>() : Ring.subspan(Offset + sizeof(ItemRecord), Length - sizeof(ItemRecord));
        const error_t Done = Apply(Record, Body, Items, Got, Tell);
        if (Done != OK && Tell.Refused) {
            std::uint16_t Property = 0;
            if ((Record.Op == ITEM_OP_CHANGE || Record.Op == ITEM_OP_RESET) && Body.size() >= sizeof(ItemChange)) {
                ItemChange What{};
                std::memcpy(&What, Body.data(), sizeof(What));
                Property = What.Property;
            }
            Tell.Refused(Record.Item, Record.Op, Property, Done);
        }
        if (Record.Op != ITEM_OP_PAD) Got.Records++;
        At += Length;
    }
    Got.Bytes = static_cast<std::uint32_t>(Whole - Read);
    __atomic_store_n(&Head.Read, Whole, __ATOMIC_RELEASE);
    return Got;
}

bool AddEvent(ItemAreaHeader &Head, std::span<std::uint8_t> Events, std::uint16_t Kind, std::uint32_t Item,
              const void *Payload, std::uint32_t Bytes) noexcept
{
    const std::uint32_t Length = (static_cast<std::uint32_t>(sizeof(ItemEvent)) + Bytes + 7u) & ~7u;
    std::uint64_t       Write  = Head.EventWrite;
    const std::uint64_t Read   = __atomic_load_n(&Head.EventRead, __ATOMIC_ACQUIRE);
    std::uint8_t *At = item_ring_place(Events.data(), static_cast<std::uint32_t>(Events.size()), &Write, Read, Length);
    if (!At || Length / 8u > 0xFFFFu) {
        __atomic_add_fetch(&Head.Lost, 1u, __ATOMIC_RELAXED);
        return false;
    }
    const ItemEvent Event{Kind, static_cast<std::uint16_t>(Length / 8u), Item};
    std::memcpy(At, &Event, sizeof(Event));
    if (Bytes) std::memcpy(At + sizeof(Event), Payload, Bytes);
    std::memset(At + sizeof(Event) + Bytes, 0, Length - sizeof(Event) - Bytes);
    __atomic_store_n(&Head.EventWrite, Write, __ATOMIC_RELEASE);
    return true;
}

}  // namespace items
