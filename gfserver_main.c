// ...existing code...
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include "gfserver-student.h"

#define BUFSIZE 512

static const char *USAGE =
    "usage:\n"
    "  gfserver_main [options]\n"
    "options:\n"
    "  -h                  Show this help message.\n"
    "  -t [nthreads]       Number of threads (Default: 20)\n"
    "  -m [content_file]   Content file mapping keys to content files (Default: content.txt)\n"
    "  -p [listen_port]    Listen port (Default: 10880)\n"
    "  -d [delay]          Delay in content_get, default 0, range 0-5000000 (microseconds)\n";

static struct option gLongOptions[] = {
    {"help", no_argument, NULL, 'h'},
    {"delay", required_argument, NULL, 'd'},
    {"nthreads", required_argument, NULL, 't'},
    {"content", required_argument, NULL, 'm'},
    {"port", required_argument, NULL, 'p'},
    {NULL, 0, NULL, 0}};

extern unsigned long int content_delay;
extern gfh_error_t gfs_handler(gfcontext_t **ctx, const char *path, void *arg);

static void _sig_handler(int signo) {
  if ((SIGINT == signo) || (SIGTERM == signo)) {
    cleanup_threads();
    exit(signo);
  }
}

/* global work queue used in the student's main */
steque_t *work_queue = NULL;

void set_pthreads(size_t nthreads); /* implemented in handler.c */

int main(int argc, char **argv) {
  int nthreads = 20;
  int option_char = 0;
  unsigned short port = 10880;
  char *content_map = "content.txt";

  setbuf(stdout, NULL);

  if (SIG_ERR == signal(SIGINT, _sig_handler)) {
    fprintf(stderr, "Can't catch SIGINT...exiting.\n");
    exit(EXIT_FAILURE);
  }

  if (SIG_ERR == signal(SIGTERM, _sig_handler)) {
    fprintf(stderr, "Can't catch SIGTERM...exiting.\n");
    exit(EXIT_FAILURE);
  }

  while ((option_char = getopt_long(argc, argv, "p:d:hm:t:", gLongOptions,
                                    NULL)) != -1) {
    switch (option_char) {
      case 'h':  /* help */
        fprintf(stdout, "%s", USAGE);
        exit(0);
        break;
      case 'p':  /* listen-port */
        port = atoi(optarg);
        break;
      case 't':  /* nthreads */
        nthreads = atoi(optarg);
        break;
      case 'm':  /* file-path */
        content_map = optarg;
        break;
      case 'd':  /* delay */
        content_delay = (unsigned long int)atoi(optarg);
        break;
      default:
        fprintf(stderr, "%s", USAGE);
        exit(1);
    }
  }

  if (nthreads < 1) nthreads = 1;

  if (content_delay > 5000000) {
    fprintf(stderr, "Content delay must be less than 5000000 (microseconds)\n");
    exit(__LINE__);
  }

  content_init(content_map);

  /* Initialize threads and work queue */
  work_queue = malloc(sizeof(*work_queue));
  if (!work_queue) { fprintf(stderr, "malloc failed\n"); exit(1); }
  steque_init(work_queue);
  set_pthreads(nthreads);

  /* create server */
  gfserver_t *gfs = gfserver_create();
  gfserver_set_port(&gfs, port);
  gfserver_set_maxpending(&gfs, 20);
  gfserver_set_handler(&gfs, gfs_handler);
  gfserver_set_handlerarg(&gfs, work_queue);
  gfserver_serve(&gfs);

  cleanup_threads();
  content_destroy();
  return 0;
}
