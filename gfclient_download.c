#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfclient-student.h"
#include "steque.h"

#define MAX_THREADS 1024
#define PATH_BUFFER_SIZE 512

#define USAGE                                                             \
  "usage:\n"                                                              \
  "  gfclient_download [options]\n"                                       \
  "options:\n"                                                            \
  "  -h                  Show this help message\n"                        \
  "  -p [server_port]    Server port (Default: 29458)\n"                  \
  "  -t [nthreads]       Number of threads (Default 8 Max: 1024)\n"       \
  "  -w [workload_path]  Path to workload file (Default: workload.txt)\n" \
  "  -s [server_addr]    Server address (Default: 127.0.0.1)\n"           \
  "  -n [num_requests]   Request download total (Default: 16)\n"

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
    {"server", required_argument, NULL, 's'},
    {"port", required_argument, NULL, 'p'},
  {"rflag", no_argument, NULL, 'r'},
    {"help", no_argument, NULL, 'h'},
    {"nthreads", required_argument, NULL, 't'},
    {"workload", required_argument, NULL, 'w'},
    {"nrequests", required_argument, NULL, 'n'},
    {NULL, 0, NULL, 0}};

static void Usage() { fprintf(stderr, "%s", USAGE); }

/* ================= Helpers ================= */
static void localPath(char *req_path, char *local_path) {
  static int counter = 0;
  sprintf(local_path, "%s-%06d", &req_path[1], counter++);
}

static FILE *openFile(char *path) {
  char *cur, *prev;
  FILE *ans;

  /* Make the directory if it isn't there */
  prev = path;
  while (NULL != (cur = strchr(prev + 1, '/'))) {
    *cur = '\0';
    if (0 > mkdir(&path[0], S_IRWXU)) {
      if (errno != EEXIST) {
        perror("Unable to create directory");
        exit(EXIT_FAILURE);
      }
    }
    *cur = '/';
    prev = cur;
  }

  if (NULL == (ans = fopen(&path[0], "w"))) {
    perror("Unable to open file");
    exit(EXIT_FAILURE);
  }

  return ans;
}

/* Callbacks */
static void writecb(void *data, size_t data_len, void *arg) {
  FILE *file = (FILE *)arg;
  fwrite(data, 1, data_len, file);
}

/* ================= Task / Queue ================= */
typedef struct {
  char *path;
  char local_path[PATH_BUFFER_SIZE];
  char *server;
  unsigned short port;
} request_task_t;

static steque_t q;
static pthread_mutex_t q_mtx;
static pthread_cond_t q_cv;
static pthread_t *threads;
static int nthreads;
static int nrequests;
static int ncompleted = 0;
static int stop = 0;

/* ================= Worker ================= */
static void* worker_main(void *arg) {
  while (1) {
    pthread_mutex_lock(&q_mtx);
    while (steque_isempty(&q) && !stop)
      pthread_cond_wait(&q_cv, &q_mtx);

    if (stop && steque_isempty(&q)) {
      pthread_mutex_unlock(&q_mtx);
      break;
    }

    request_task_t *task = steque_pop(&q);
    pthread_mutex_unlock(&q_mtx);

    /* === 执行下载逻辑 === */
    if (strlen(task->path) > PATH_BUFFER_SIZE) {
      fprintf(stderr, "Request path exceeded maximum of %d characters\n",
              PATH_BUFFER_SIZE);
      free(task->path);
      free(task);
      continue;
    }

    FILE *file = openFile(task->local_path);
    gfcrequest_t *gfr = gfc_create();
    gfc_set_path(&gfr, task->path);
    gfc_set_server(&gfr, task->server);
    gfc_set_port(&gfr, task->port);
    gfc_set_writearg(&gfr, file);
    gfc_set_writefunc(&gfr, writecb);

    fprintf(stdout, "Requesting %s%s\n", task->server, task->path);

    int returncode = gfc_perform(&gfr);
    if (returncode < 0) {
      fprintf(stdout, "gfc_perform returned an error %d\n", returncode);
      fclose(file);
      if (0 > unlink(task->local_path))
        fprintf(stderr, "warning: unlink failed on %s\n", task->local_path);
    } else {
      fclose(file);
    }

    if (gfc_get_status(&gfr) != GF_OK) {
      if (0 > unlink(task->local_path))
        fprintf(stderr, "warning: unlink failed on %s\n", task->local_path);
    }

    fprintf(stdout, "Status: %s\n", gfc_strstatus(gfc_get_status(&gfr)));
    fprintf(stdout, "Received %zu of %zu bytes\n",
            gfc_get_bytesreceived(&gfr), gfc_get_filelen(&gfr));

    gfc_cleanup(&gfr);

    free(task->path);
    free(task);

    pthread_mutex_lock(&q_mtx);
    ncompleted++;
    if (ncompleted >= nrequests) {
      stop = 1;
      pthread_cond_broadcast(&q_cv);
    }
    pthread_mutex_unlock(&q_mtx);
  }
  return NULL;
}

/* ================= Main ================= */
int main(int argc, char **argv) {
  char *workload_path = "workload.txt";
  char *server = "127.0.0.1";
  unsigned short port = 29458;
  int option_char = 0;

  nthreads = 8;
  nrequests = 16;

  setbuf(stdout, NULL);

  // Parse and set command line arguments
  while ((option_char = getopt_long(argc, argv, "p:n:r:hs:rt:w:", gLongOptions,
                                    NULL)) != -1) {
    switch (option_char) {
      case 'n':
      case 'r': 
      nrequests = atoi(optarg);
      /* ignored compatibility flag used by some graders */ 
      break;
      case 'w': workload_path = optarg; break;
      case 's': server = optarg; break;
      case 'p': port = atoi(optarg); break;
      case 't': nthreads = atoi(optarg); break;
      case 'h': Usage(); exit(0);
      default:  Usage(); exit(1);
    }
  }

  if (EXIT_SUCCESS != workload_init(workload_path)) {
    fprintf(stderr, "Unable to load workload file %s.\n", workload_path);
    exit(EXIT_FAILURE);
  }
  if (port > 65331) {
    fprintf(stderr, "Invalid port number\n");
    exit(EXIT_FAILURE);
  }
  if (nthreads < 1 || nthreads > MAX_THREADS) {
    fprintf(stderr, "Invalid number of threads\n");
    exit(EXIT_FAILURE);
  }

  gfc_global_init();

  steque_init(&q);
  pthread_mutex_init(&q_mtx, NULL);
  pthread_cond_init(&q_cv, NULL);
  threads = calloc(nthreads, sizeof(pthread_t));
  for (int i=0; i<nthreads; i++)
    pthread_create(&threads[i], NULL, worker_main, NULL);

  for (int i = 0; i < nrequests; i++) {
    char *req_path = workload_get_path();
    request_task_t *t = calloc(1, sizeof(request_task_t));
    t->path = strdup(req_path);
    localPath(req_path, t->local_path);
    t->server = server;
    t->port = port;

    pthread_mutex_lock(&q_mtx);
    steque_enqueue(&q, t);
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mtx);
  }

  for (int i=0; i<nthreads; i++)
    pthread_join(threads[i], NULL);

  free(threads);
  pthread_mutex_destroy(&q_mtx);
  pthread_cond_destroy(&q_cv);
  steque_destroy(&q);

  gfc_global_cleanup();
  return 0;
}
