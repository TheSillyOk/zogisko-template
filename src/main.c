#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include "zygisk.h"

#include "logger.h"

#include <jni.h>

struct api_table *api_table;
JNIEnv *java_env;

char *get_string_data(JNIEnv *env, jstring *value) {
  const char *str = (*env)->GetStringUTFChars(env, *value, 0);
  if (str == NULL) return NULL;

  char *out = strdup(str);
  (*env)->ReleaseStringUTFChars(env, *value, str);

  return out;
}

void pre_specialize(const char *process) {
  /* Demonstrate connecting to to companion process
  We ask the companion for a random number and a sum
  of all random numbers sent so far */
  unsigned r = 0;
  unsigned n = 0;

  int fd = api_table->connectCompanion(api_table->impl);
  if (fd == -1) {
    LOGE("Failed to connect to companion");
    return;
  }

  read(fd, &r, 1); // receive random number
  write(fd, &r, 1); // reply upon number receival

  read(fd, &n, sizeof(n)); // also get saved number
  write(fd, &n, sizeof(n)); // reply back to companion

  close(fd);

  LOGI("process=[%s], r=[%u], n=[%u]", process, r, n);

  // Since we do not hook any functions, we should let Zygisk dlclose ourselves
  api_table->setOption(api_table->impl, DLCLOSE_MODULE_LIBRARY);
}

/* INFO: This is the beginning of zygisk's functions
those are triggered by it on each stage:
 * .preAppSpecialize: Called on app open; runs with elevated privileges before the process is sandboxed.
 * .postAppSpecialize: Called after app sandboxing; runs within the app's restricted security context.
 * .preServerSpecialize: Called before system_server forks; used for system-level modifications.
 * .postServerSpecialize: Called after system_server is specialized; runs with system-level privileges.
*/
void pre_app_specialize(void *mod_data, struct AppSpecializeArgs *args) {
  (void) mod_data;

  char *process = get_string_data(java_env, args->nice_name);

  if (process != NULL) pre_specialize(process);

  free((void*)process);
}

void post_app_specialize(void *mod_data, const struct AppSpecializeArgs *args) {
  (void) mod_data; (void) args;
}

void pre_server_specialize(void *mod_data, struct ServerSpecializeArgs *args) {
  (void) mod_data; (void) args;

  pre_specialize("system_server");
}

void post_server_specialize(void *mod_data, const struct ServerSpecializeArgs *args) {
  (void) mod_data; (void) args;
}

void zygisk_module_entry(struct api_table *table, JNIEnv *env) {
  api_table = table;
  java_env = env;

  static struct module_abi abi = {
    .api_version = 5,
    .impl = (void *)"zygisk_example",
    .preAppSpecialize = pre_app_specialize,
    .postAppSpecialize = post_app_specialize,
    .preServerSpecialize = pre_server_specialize,
    .postServerSpecialize = post_server_specialize
  };

  if (!table->registerModule(table, &abi)) return;
}

void zygisk_companion_entry(int fd) {
  static int urandom;
  static unsigned n;
  unsigned r = 0;

  if (urandom <= 0) {
    urandom = open("/dev/urandom", O_RDONLY);
    LOGD("companion urandom open [%d]", urandom);
  }

  if (urandom == -1) {
    LOGE("companion failed to open urandom");
    goto close;
  }

  read(urandom, &r, 1); // one random byte (0-255)
  r = (r % 9) + 1; // turn into 1-9 range using modulus

  LOGI("companion r=[%u]", r);
  write(fd, &r, 1); // send random number

  size_t reply = read(fd, &r, 1); // wait for reply
  if (reply != 1) {
    LOGE("companion received unexpected amount of bytes for random [%zu]", reply);
    goto close;
  }

  n = (n + r) % 1000; // saved number + random number
  // modulus is used to limit it to 999

  LOGI("companion n=[%u]", n);
  write(fd, &n, sizeof(n)); // send saved number

  reply = read(fd, &r, sizeof(4)); // reuse "r" & "reply"
  if (reply <= 0) {
    LOGE("companion received unexpected amount of bytes for number [%zu]", reply);
    goto close;
  }

close:
  close(fd);
  return;
}
