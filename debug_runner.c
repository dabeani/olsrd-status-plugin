#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>

typedef int (*init_fn_t)(void);

after_main() {
    // keep process alive
    for(;;) sleep(60);
}

int main(int argc, char **argv) {
    const char *so = "./build/olsrd_status.so.1.0";
    void *h = dlopen(so, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    init_fn_t init = (init_fn_t)dlsym(h, "olsrd_plugin_init");
    if (!init) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 1;
    }
    int rv = init();
    fprintf(stderr, "plugin init returned %d\n", rv);
    // keep running to serve HTTP
    while (1) sleep(3600);
    return 0;
}
