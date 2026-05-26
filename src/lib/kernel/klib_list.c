/* klib_list.c — generic doubly-linked list with an internal spinlock.
 *
 * Every mutating operation acquires the list lock BEFORE inspecting head /
 * tail. An unlocked NULL-check shortcut races with a concurrent push from
 * a peer CPU and either loses the just-pushed item or dereferences a NULL
 * tail re-read inside the lock — both bugs were present and fixed during
 * the AMP correctness sweep. */
#include "klib.h"

void list_init(list_t *list)
{
    if (!list)
        return;

    memset(list, 0, sizeof(list_t));
    spinlock_init(&list->lock);
}

void list_destroy(list_t *list)
{
    if (!list)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        list_node_t *next = current->next;
        kfree(current);
        current = next;
    }

    list->head = list->tail = NULL;
    list->size = 0;

    spin_unlock(&list->lock);
}

void list_push_back(list_t *list, void *data)
{
    if (!list)
        return;

    list_node_t *node = kmalloc(sizeof(list_node_t));
    if (!node)
        return;

    node->data = data;
    node->next = NULL;

    spin_lock(&list->lock);

    node->prev = list->tail;
    if (list->tail)
        list->tail->next = node;
    else
        list->head = node;
    list->tail = node;
    list->size++;

    spin_unlock(&list->lock);
}

void *list_pop_back(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);

    list_node_t *node = list->tail;
    if (!node)
    {
        spin_unlock(&list->lock);
        return NULL;
    }
    void *data = node->data;

    if (node->prev)
    {
        node->prev->next = NULL;
        list->tail = node->prev;
    }
    else
    {
        list->head = list->tail = NULL;
    }

    list->size--;
    kfree(node);

    spin_unlock(&list->lock);
    return data;
}

void list_push_front(list_t *list, void *data)
{
    if (!list)
        return;

    list_node_t *node = kmalloc(sizeof(list_node_t));
    if (!node)
        return;

    node->data = data;
    node->prev = NULL;

    spin_lock(&list->lock);

    node->next = list->head;
    if (list->head)
        list->head->prev = node;
    else
        list->tail = node;
    list->head = node;
    list->size++;

    spin_unlock(&list->lock);
}

void *list_pop_front(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);

    list_node_t *node = list->head;
    if (!node)
    {
        spin_unlock(&list->lock);
        return NULL;
    }
    void *data = node->data;

    if (node->next)
    {
        node->next->prev = NULL;
        list->head = node->next;
    }
    else
    {
        list->head = list->tail = NULL;
    }

    list->size--;
    kfree(node);

    spin_unlock(&list->lock);
    return data;
}

void *list_front(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);
    void *data = list->head ? list->head->data : NULL;
    spin_unlock(&list->lock);

    return data;
}

void *list_back(list_t *list)
{
    if (!list)
        return NULL;

    spin_lock(&list->lock);
    void *data = list->tail ? list->tail->data : NULL;
    spin_unlock(&list->lock);

    return data;
}

bool list_empty(list_t *list)
{
    if (!list)
        return true;

    spin_lock(&list->lock);
    bool empty = (list->size == 0);
    spin_unlock(&list->lock);

    return empty;
}

size_t list_size(list_t *list)
{
    if (!list)
        return 0;

    spin_lock(&list->lock);
    size_t size = list->size;
    spin_unlock(&list->lock);

    return size;
}

void list_remove(list_t *list, void *data, bool (*cmp)(void *, void *))
{
    if (!list || !data || !cmp)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        if (cmp(current->data, data))
        {
            list_node_t *to_remove = current;

            if (to_remove->prev)
                to_remove->prev->next = to_remove->next;
            else
                list->head = to_remove->next;

            if (to_remove->next)
                to_remove->next->prev = to_remove->prev;
            else
                list->tail = to_remove->prev;

            list->size--;
            current = to_remove->next;
            kfree(to_remove);
        }
        else
        {
            current = current->next;
        }
    }

    spin_unlock(&list->lock);
}

void list_for_each(list_t *list, void (*func)(void *))
{
    if (!list || !func)
        return;

    spin_lock(&list->lock);

    list_node_t *current = list->head;
    while (current)
    {
        func(current->data);
        current = current->next;
    }

    spin_unlock(&list->lock);
}
