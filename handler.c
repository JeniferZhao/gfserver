#include "gfserver-student.h"
#include "steque.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define BUFSIZE (64 * 1024)

/* forward decl */
static void *worker_main(void *arg);

/* globals */
static steque_t queue;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t *workers = NULL;
static size_t nworkers = 0;
static int stopping = 0;

/* Send file in chunks strictly up to st_size. Abort on short read/write. */
static gfh_error_t serve_file(gfcontext_t **ctxpp, const char *path) {
    int fd = content_get(path);
    if (fd < 0) {
        /* Normal 404 path */
        if (gfs_sendheader(ctxpp, GF_FILE_NOT_FOUND, 0) < 0) {
            gfs_abort(ctxpp);
            return gfh_failure;
        }
        return gfh_success;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        (void) gfs_sendheader(ctxpp, GF_ERROR, 0);
        gfs_abort(ctxpp);
        close(fd);
        return gfh_failure;
    }

    size_t total = (size_t) st.st_size;
    if (gfs_sendheader(ctxpp, GF_OK, total) < 0) {
        gfs_abort(ctxpp);
        close(fd);
        return gfh_failure;
    }

    char buf[BUFSIZE];
    size_t sent = 0;
    while (sent < total) {
        size_t toread = total - sent;
        if (toread > sizeof(buf)) toread = sizeof(buf);

        ssize_t r = pread(fd, buf, toread, (off_t)sent);
        if (r < 0) {
            if (errno == EINTR) continue;
            gfs_abort(ctxpp);
            close(fd);
            return gfh_failure;
        }
        if (r == 0) break; /* unexpected EOF */

        ssize_t w = gfs_send(ctxpp, buf, (size_t)r);
        if (w < 0 || (size_t)w != (size_t)r) {
            gfs_abort(ctxpp);
            close(fd);
            return gfh_failure;
        }
        sent += (size_t)r;
    }

    if (sent != total) {
        gfs_abort(ctxpp);
        close(fd);
        return gfh_failure;
    }

    close(fd);
    return gfh_success;
}

/* worker thread main: pop one request and serve without holding the lock */
static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_mtx);
        while (steque_isempty(&queue) && !stopping)
            pthread_cond_wait(&q_cv, &q_mtx);

        if (stopping && steque_isempty(&queue)) {
            pthread_mutex_unlock(&q_mtx);
            break;
        }

        steque_request *req = (steque_request *) steque_pop(&queue);
        pthread_mutex_unlock(&q_mtx);
        if (!req) continue;

        gfcontext_t **ctxpp = req->ctx_slot;
        if (ctxpp != NULL) {
            *ctxpp = req->ctx;
            (void) serve_file(ctxpp, req->filepath);
            *ctxpp = NULL;
        }

        /* cleanup */
        free(req->filepath);
        req->filepath = NULL;
        req->ctx = NULL;
        req->ctx_slot = NULL;
        free(req);
    }
    return NULL;
}

void set_pthreads(size_t numthreads) {
    if (numthreads < 1) numthreads = 1;
    nworkers = numthreads;
    workers = (pthread_t *) calloc(nworkers, sizeof(pthread_t));
    steque_init(&queue);

    for (size_t i = 0; i < nworkers; ++i) {
        if (pthread_create(&workers[i], NULL, worker_main, NULL) != 0) {
            pthread_mutex_lock(&q_mtx);
            stopping = 1;
            pthread_cond_broadcast(&q_cv);
            pthread_mutex_unlock(&q_mtx);
            for (size_t j = 0; j < i; ++j) pthread_join(workers[j], NULL);
            free(workers); workers = NULL; nworkers = 0;
            break;
        }
    }
}

void cleanup_threads(void) {
    pthread_mutex_lock(&q_mtx);
    stopping = 1;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mtx);

    if (workers) {
        for (size_t i = 0; i < nworkers; ++i) pthread_join(workers[i], NULL);
        free(workers); workers = NULL; nworkers = 0;
    }

    while (!steque_isempty(&queue)) {
        steque_request *r = (steque_request *) steque_pop(&queue);
        if (r) {
            if (r->ctx_slot != NULL) {
                *(r->ctx_slot) = r->ctx;
                gfs_abort(r->ctx_slot);
                *(r->ctx_slot) = NULL;
            }
            free(r->filepath);
            r->filepath = NULL;
            r->ctx = NULL;
            r->ctx_slot = NULL;
            free(r);
        }
    }
    steque_destroy(&queue);

    pthread_mutex_destroy(&q_mtx);
    pthread_cond_destroy(&q_cv);
}

/* Handler: capture the library context pointer, enqueue, return success */
gfh_error_t gfs_handler(gfcontext_t **ctx, const char *path, void *arg) {
    (void)arg;

    steque_request *req = (steque_request *) calloc(1, sizeof(*req));
    if (!req) {
        (void) gfs_sendheader(ctx, GF_ERROR, 0);
        gfs_abort(ctx);
        return gfh_failure;
    }

    req->filepath = strdup(path ? path : "/");
    if (!req->filepath) {
        free(req);
        (void) gfs_sendheader(ctx, GF_ERROR, 0);
        gfs_abort(ctx);
        return gfh_failure;
    }

    req->ctx = *ctx;
    req->ctx_slot = ctx;

    /* Detach from the library thread context before returning */
    *ctx = NULL;

    pthread_mutex_lock(&q_mtx);
    steque_enqueue(&queue, (steque_item) req);
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mtx);

    return gfh_success;
}
