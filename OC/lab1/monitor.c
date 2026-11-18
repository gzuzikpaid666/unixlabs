#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENTS_COUNT 10

typedef struct {
    int   id;
    char *payload;
} Event;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  condition;
    bool            ready;
    bool            stopping;
    void           *data;
} Monitor;

static Monitor mon = {
    .mutex     = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER,
    .ready     = false,
    .stopping  = false,
    .data      = NULL
};


static void provide(Monitor *m, Event *ev)
{
    pthread_mutex_lock(&m->mutex);
    while (m->ready)
        pthread_cond_wait(&m->condition, &m->mutex);

    m->data  = ev;
    m->ready = true;
    printf("[Поставщик]   событие #%d отправлено\n", ev->id);
    fflush(stdout);


    pthread_cond_broadcast(&m->condition);
    pthread_mutex_unlock(&m->mutex);
}

static void stop(Monitor *m)
{
    pthread_mutex_lock(&m->mutex);
    m->stopping = true;
    pthread_cond_broadcast(&m->condition);
    pthread_mutex_unlock(&m->mutex);
}

static Event *consume(Monitor *m)
{
    pthread_mutex_lock(&m->mutex);
    while (!m->ready && !m->stopping)
        pthread_cond_wait(&m->condition, &m->mutex);

    Event *ev = NULL;
    if (m->ready) {
        ev = m->data;
        m->data  = NULL;
        m->ready = false;
        printf("[Потребитель] событие #%d получено\n", ev->id);
        fflush(stdout);
        pthread_cond_broadcast(&m->condition);
    }
    pthread_mutex_unlock(&m->mutex);
    return ev;
}

static void *producer(void *arg)
{
    Monitor *m = arg;
    for (int i = 1; i <= EVENTS_COUNT; i++) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        Event *ev = malloc(sizeof *ev);
        if (!ev) { perror("malloc"); break; }
        ev->id      = i;
        ev->payload = strdup("данные события");
        provide(m, ev);
    }
    stop(m);
    return NULL;
}

static void *consumer(void *arg)
{
    Monitor *m = arg;
    Event *ev;
    while ((ev = consume(m)) != NULL) {
        free(ev->payload);
        free(ev);
    }
    return NULL;
}

int main(void)
{
    pthread_t prod, cons;

    pthread_create(&cons, NULL, consumer, &mon);
    pthread_create(&prod, NULL, producer, &mon);

    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    pthread_cond_destroy(&mon.condition);
    pthread_mutex_destroy(&mon.mutex);
    return 0;
}
