#ifndef __GF_SERVER_STUDENT_H__
#define __GF_SERVER_STUDENT_H__

#include "gf-student.h"
#include "gfserver.h"
#include "content.h"
#include "steque.h"
#include <pthread.h>

/* Request object stored on the work queue */
typedef struct steque_request {
    char *filepath;         /* heap-allocated copy of the requested path */
    void *arg;              /* optional / unused */
    gfcontext_t *ctx;       /* library-provided context pointer */
    gfcontext_t **ctx_slot; /* address of the library's ctx pointer */
} steque_request;

void set_pthreads(size_t nthreads);
void cleanup_threads(void);

/* Handler called by the gfserver library for each request */
gfh_error_t gfs_handler(gfcontext_t **ctx, const char *path, void *arg);

#endif /* __GF_SERVER_STUDENT_H__ */
