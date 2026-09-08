#pragma once

#include <kernel/compiler.h>
#include <kernel/spinlock.h>

struct thread;

struct waitqueue {
    struct wait_entry *head;
};

struct wait_entry {
    struct thread *thread;
    struct waitqueue *queue;
    struct wait_entry *next;
    struct wait_entry *chain;
    bool woken;
};

#define WAITQUEUE_INIT { NULL }

void waitqueue_init(struct waitqueue *wq);

/*
 * Dorme na fila. Deve ser chamado com interrupcoes desligadas e com `lock`
 * segurado (o mesmo que protege a condicao); a trava e solta antes da troca de
 * contexto e retomada na volta. Devolve true se acordou por wake_up.
 */
bool wait_sleep(struct waitqueue *wq, spinlock_t *lock);
bool wait_sleep_timeout(struct waitqueue *wq, spinlock_t *lock, uint64_t ms);

#define wait_event(wq, lock, cond)                  \
    do {                                            \
        while (!(cond))                             \
            wait_sleep(&(wq), (lock));              \
    } while (0)

void wake_up(struct waitqueue *wq);
void wake_up_all(struct waitqueue *wq);
void wake_up_all_locked(struct waitqueue *wq);

/* Chamado pelo escalonador quando a thread acorda por outro motivo. */
void wait_unlink(struct thread *t);

/* Espera em varias filas ao mesmo tempo (base do poll). */
#define POLL_TABLE_MAX 32

struct poll_table {
    struct wait_entry entries[POLL_TABLE_MAX];
    int count;
    bool registering;
};

void poll_table_init(struct poll_table *pt);
void poll_wait(struct poll_table *pt, struct waitqueue *wq);
bool poll_table_sleep(struct poll_table *pt, uint64_t ms);
void poll_table_clear(struct poll_table *pt);
