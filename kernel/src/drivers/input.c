#include <drivers/input.h>
#include <kernel/file.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/signal.h>
#include <kernel/time.h>
#include <kernel/syscall.h>

#define QUEUE_SIZE  256

static spinlock_t lock = SPINLOCK_INIT;
static struct input_event queue[QUEUE_SIZE];
static uint32_t head, tail;
static struct waitqueue readers = WAITQUEUE_INIT;
static uint32_t grabs;
static uint32_t timeout_ms;
static uint64_t events;
static uint64_t dropped;

static void push(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint32_t next = (head + 1) % QUEUE_SIZE;
    if (next == tail) {
        tail = (tail + 1) % QUEUE_SIZE;
        dropped++;
    }
    queue[head].type = type;
    queue[head].a = a;
    queue[head].b = b;
    queue[head].c = c;
    head = next;
    events++;
    spin_unlock_irqrestore(&lock, flags);
    wake_up_all(&readers);
}

void input_init(void)
{
    head = tail = 0;
    grabs = 0;
    timeout_ms = 0;
}

bool input_post_key(uint8_t scancode, char ascii, uint8_t modifiers, bool pressed)
{
    if (!grabs)
        return false;
    push(pressed ? INPUT_EV_KEY_DOWN : INPUT_EV_KEY_UP, scancode, (uint8_t)ascii, modifiers);
    return true;
}

void input_post_mouse_move(int32_t dx, int32_t dy, int32_t dz)
{
    if (!grabs)
        return;
    push(INPUT_EV_MOUSE_MOVE, dx, dy, dz);
}

void input_post_mouse_button(uint32_t buttons)
{
    if (!grabs)
        return;
    push(INPUT_EV_MOUSE_BUTTON, (int32_t)buttons, 0, 0);
}

bool input_grabbed(void)
{
    return grabs > 0;
}

void input_grab(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    grabs++;
    spin_unlock_irqrestore(&lock, flags);
}

void input_release(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    if (grabs > 0)
        grabs--;
    if (grabs == 0) {
        head = tail = 0;
        timeout_ms = 0;
    }
    spin_unlock_irqrestore(&lock, flags);
}

static uint64_t drain(void *buf, uint64_t len)
{
    struct input_event *out = buf;
    uint64_t max = len / sizeof(*out);
    uint64_t n = 0;
    uint64_t flags = spin_lock_irqsave(&lock);
    while (n < max && tail != head) {
        out[n++] = queue[tail];
        tail = (tail + 1) % QUEUE_SIZE;
    }
    spin_unlock_irqrestore(&lock, flags);
    return n;
}

int64_t input_read(void *buf, uint64_t len)
{
    if (len < sizeof(struct input_event))
        return -EINVAL;

    uint32_t limit = timeout_ms;
    uint64_t deadline = time_uptime_ms() + limit;
    for (;;) {
        uint64_t n = drain(buf, len);
        if (n)
            return (int64_t)(n * sizeof(struct input_event));
        if (limit && time_uptime_ms() >= deadline)
            return 0;

        uint64_t flags = spin_lock_irqsave(&lock);
        if (tail != head) {
            spin_unlock_irqrestore(&lock, flags);
            continue;
        }
        uint64_t wait_ms = 0;
        if (limit) {
            uint64_t now = time_uptime_ms();
            wait_ms = now >= deadline ? 1 : deadline - now;
        }
        wait_sleep_intr(&readers, &lock, wait_ms);
        spin_unlock_irqrestore(&lock, flags);
        if (signal_pending_current())
            return -EINTR;
    }
}

short input_poll(struct poll_table *pt)
{
    poll_wait(pt, &readers);
    uint64_t flags = spin_lock_irqsave(&lock);
    short revents = tail != head ? POLLIN : 0;
    spin_unlock_irqrestore(&lock, flags);
    return revents;
}

void input_set_timeout(uint32_t ms)
{
    timeout_ms = ms;
}

uint32_t input_timeout(void)
{
    return timeout_ms;
}

void input_flush(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    head = tail = 0;
    spin_unlock_irqrestore(&lock, flags);
}

uint32_t input_queued(void)
{
    return (head - tail + QUEUE_SIZE) % QUEUE_SIZE;
}

uint64_t input_event_count(void)
{
    return events;
}

uint64_t input_dropped_count(void)
{
    return dropped;
}

static int64_t input_file_read(struct file *f, void *buf, uint64_t len)
{
    (void)f;
    return input_read(buf, len);
}

static void input_file_close(struct file *f)
{
    (void)f;
    input_release();
}

static long input_file_ioctl(struct file *f, uint64_t request, uint64_t arg)
{
    (void)f;
    switch (request) {
    case INPUTIOC_SETTIMEOUT:
        input_set_timeout((uint32_t)arg);
        return 0;
    case INPUTIOC_FLUSH:
        input_flush();
        return 0;
    default:
        return -EINVAL;
    }
}

static short input_file_poll(struct file *f, struct poll_table *pt)
{
    (void)f;
    return input_poll(pt);
}

static const struct file_ops input_ops = {
    .read = input_file_read,
    .close = input_file_close,
    .ioctl = input_file_ioctl,
    .poll = input_file_poll,
};

struct file *input_open(void)
{
    struct file *f = file_alloc(&input_ops, NULL);
    if (f)
        input_grab();
    return f;
}
