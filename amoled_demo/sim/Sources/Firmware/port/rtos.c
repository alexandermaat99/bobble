// FreeRTOS on POSIX threads. Tasks are threads (priorities and cores are ignored: the Mac has plenty of both), a tick
// is a millisecond, and semaphores, queues and task notifications are a mutex and a condition variable each.
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"

struct sim_task {
    TaskFunction_t fn;
    void *arg;
    char name[16];
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t notify;
};

static __thread struct sim_task *self;

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    static int64_t t0;
    int64_t t = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
    if (!t0) t0 = t;
    return t - t0 + 1;                                // (like the board: time since start-up)
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000); }

// A deadline for a wait of `ticks` (portMAX_DELAY: none)
static void deadline(TickType_t ticks, struct timespec *ts)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ns = tv.tv_usec * 1000LL + (int64_t)ticks * 1000000LL;
    ts->tv_sec = tv.tv_sec + ns / 1000000000LL;
    ts->tv_nsec = ns % 1000000000LL;
}

// Wait on c (with m held) until woken or the wait runs out; false: timed out
static bool cond_wait(pthread_cond_t *c, pthread_mutex_t *m, TickType_t ticks, const struct timespec *until)
{
    if (ticks == portMAX_DELAY) { pthread_cond_wait(c, m); return true; }
    return pthread_cond_timedwait(c, m, until) != ETIMEDOUT;
}

static struct sim_task *task_new(TaskFunction_t fn, void *arg, const char *name)
{
    struct sim_task *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    strlcpy(t->name, name ? name : "task", sizeof t->name);
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    return t;
}

static void *task_main(void *p)
{
    self = p;
    pthread_setname_np(self->name);
    self->fn(self->arg);
    return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *out, BaseType_t core)
{
    struct sim_task *t = task_new(fn, arg, name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1 << 20);           // (the board's stacks are sized for its code: be generous)
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &a, task_main, t)) return pdFAIL;
    if (out) *out = t;
    return pdPASS;
}

// The thread app_main runs on (and any other thread that calls in) gets a task of its own on first use
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    if (!self) self = task_new(NULL, NULL, "main");
    return self;
}

void vTaskDelete(TaskHandle_t t)
{
    if (!t || t == self) pthread_exit(NULL);
}

void vTaskDelay(TickType_t ticks) { usleep((useconds_t)ticks * 1000); }

BaseType_t xTaskNotifyGive(TaskHandle_t t)
{
    if (!t) return pdFAIL;
    pthread_mutex_lock(&t->m);
    t->notify++;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    struct sim_task *t = xTaskGetCurrentTaskHandle();
    struct timespec until;
    deadline(ticks, &until);
    pthread_mutex_lock(&t->m);
    while (!t->notify && cond_wait(&t->c, &t->m, ticks, &until)) {}
    uint32_t v = t->notify;
    if (v) t->notify = clear ? 0 : v - 1;
    pthread_mutex_unlock(&t->m);
    return v;
}

// ---- semaphores (mutex: max 1, starts given; binary: max 1, starts taken; counting)
struct sim_sem { pthread_mutex_t m; pthread_cond_t c; unsigned count, max; };

SemaphoreHandle_t sim_sem_new(unsigned max, unsigned initial)
{
    struct sim_sem *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->max = max;
    s->count = initial;
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    struct timespec until;
    deadline(ticks, &until);
    pthread_mutex_lock(&s->m);
    while (!s->count && (ticks && cond_wait(&s->c, &s->m, ticks, &until))) {}
    bool got = s->count > 0;
    if (got) s->count--;
    pthread_mutex_unlock(&s->m);
    return got ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    bool ok = s->count < s->max;
    if (ok) s->count++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return ok ? pdTRUE : pdFALSE;
}

// ---- queues
struct sim_queue { pthread_mutex_t m; pthread_cond_t c; unsigned len, item, head, n; uint8_t *buf; };

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct sim_queue *q = calloc(1, sizeof *q);
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->len = len;
    q->item = item;
    q->buf = calloc(len, item);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    struct timespec until;
    deadline(ticks, &until);
    pthread_mutex_lock(&q->m);
    while (q->n == q->len && (ticks && cond_wait(&q->c, &q->m, ticks, &until))) {}
    bool ok = q->n < q->len;
    if (ok) {
        memcpy(q->buf + ((q->head + q->n) % q->len) * q->item, item, q->item);
        q->n++;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    struct timespec until;
    deadline(ticks, &until);
    pthread_mutex_lock(&q->m);
    while (!q->n && (ticks && cond_wait(&q->c, &q->m, ticks, &until))) {}
    bool ok = q->n > 0;
    if (ok) {
        memcpy(item, q->buf + q->head * q->item, q->item);
        q->head = (q->head + 1) % q->len;
        q->n--;
        pthread_cond_broadcast(&q->c);
    }
    pthread_mutex_unlock(&q->m);
    return ok ? pdTRUE : pdFALSE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    unsigned n = q->n;
    pthread_mutex_unlock(&q->m);
    return n;
}

// ---- critical sections: one lock for all (they're short)
static pthread_mutex_t crit = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
void sim_critical(bool enter)
{
    if (enter) pthread_mutex_lock(&crit);
    else pthread_mutex_unlock(&crit);
}
