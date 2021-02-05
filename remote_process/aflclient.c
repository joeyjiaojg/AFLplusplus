#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <signal.h>
#ifdef __ANDROID__
#include "android-ashmem.h"
#endif

#define MAP_SIZE_POW2 16
#define MAP_SIZE (1 << MAP_SIZE_POW2)

#define SHM_ENV_VAR         "__AFL_SHM_ID"
#define AFL_SOCK_SUFFIX     "AFL_SOCK_SUFFIX"
#define AFL_DEBUG           "AFL_DEBUG"
#define AFL_NO_REMOTE       "AFL_NO_REMOTE"

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

static s32 shm_id = -1;
static s32 server_pid = -1;
static s32 client_pid = -1;
static char afl_debug = 0;
char* tmpdir;
static struct sockaddr_un addr;
static char sock_str[1024];
static int afl_sock_fd;

u8  __afl_area_initial[MAP_SIZE];
u8* __afl_area_ptr = __afl_area_initial;

void handle_sig(int sig) {
  if (sig == SIGTERM) { // timeout from afl-fuzz
    if (server_pid != -1)
      kill(server_pid, sig);
  }
}

/* SHM setup. */

void __afl_map_shm(void) {

  char *id_str = getenv(SHM_ENV_VAR);

  /* If we're running under AFL, attach to the appropriate region, replacing the
     early-stage __afl_area_initial region that is needed to allow some really
     hacky .init code to work correctly in projects such as OpenSSL. */

  if (id_str) {

    shm_id = atoi(id_str);

    __afl_area_ptr = shmat(shm_id, NULL, 0);

    /* Whooooops. */

    if (__afl_area_ptr == (void *)-1) _exit(1);

    /* Write something into the bitmap so that even with low AFL_INST_RATIO,
       our parent doesn't give up on us. */

    __afl_area_ptr[0] = 1;

  } else if (afl_debug) {
    shm_id = shmget(IPC_PRIVATE, MAP_SIZE, IPC_CREAT | IPC_EXCL | 0600);
    __afl_area_ptr = shmat(shm_id, NULL, 0);

    if (__afl_area_ptr == (void *)-1) _exit(1);

    __afl_area_ptr[0] = 1;
  }

}

void __afl_start_client(void) {
  tmpdir = getenv("TMPDIR");
  if (!tmpdir) tmpdir = "/tmp";

  char *sock_suffix = getenv(AFL_SOCK_SUFFIX);
  if (sock_suffix)
    snprintf(sock_str, sizeof(sock_str), "%s/afl_sock_%s", tmpdir, sock_suffix);
  else
    snprintf(sock_str, sizeof(sock_str), "%s/afl_sock", tmpdir);

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_str, sizeof(addr.sun_path));

  if ((afl_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    perror("socket create failed");
    _exit(EXIT_FAILURE);
  }
}

void afl_client_start(void) {
  if (getenv(AFL_NO_REMOTE)) return;

//RECONNECT:
  if (connect(afl_sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      _exit(1);
//    usleep(1000);
//    goto RECONNECT;
  }

  if (send(afl_sock_fd, &shm_id, 4, 0) != 4) _exit(1);
 
  if (recv(afl_sock_fd, &server_pid, 4, MSG_WAITALL) != 4) _exit(1);

  client_pid = getpid();
  if (send(afl_sock_fd, &client_pid, 4, 0) != 4) _exit(1);
}

void afl_client_exit(void);

void setup_signal_handlers(void) {
  signal(SIGTERM, handle_sig);
}

__attribute__((constructor(5)))
void afl_client_init(void) {
  static u8 init_done;

  if (getenv(AFL_DEBUG)) afl_debug = 1;
  if (getenv(AFL_NO_REMOTE)) return;

  atexit(afl_client_exit);
  setup_signal_handlers();

  if (!init_done) {

    __afl_map_shm();
    __afl_start_client();
    init_done = 1;
  }
}

void afl_client_continue(void) {
  if (afl_debug) printf("afl_client_continue\n");
  if (getenv(AFL_NO_REMOTE)) return;
  if (send(afl_sock_fd, "CONT", 4, 0) != 4) _exit(1);
}

void afl_client_exit(void) {
  u8 tmp[4];

  memset(tmp, 0, 4);
  // phone server that I will exit now
  if (send(afl_sock_fd, tmp, 4, 0) != 4) _exit(1);

#ifdef __ANDROID__
  // AFL context
  char *id_str = getenv(SHM_ENV_VAR);
  if (id_str || afl_debug) {
    if (recv(afl_sock_fd, __afl_area_ptr, MAP_SIZE, MSG_WAITALL) != MAP_SIZE) _exit(1);
    if (send(afl_sock_fd, tmp, 4, 0) != 4) _exit(1);
  }
#endif
  
  close(afl_sock_fd);

#ifdef __ANDROID__
  usleep(10000);
#endif
  
  _exit(0);
}
