#include "../inc/utils.h"

static void started_cb(enum modules module);
static void disable_module(const enum modules module);

/**
 * Create timer and returns its fd to
 * the main struct pollfd
 */
int start_timer(int clockid, int initial_timeout) {
    int timerfd = timerfd_create(clockid, 0);
    if (timerfd == -1) {
        ERROR("could not start timer: %s\n", strerror(errno));
    } else {
        set_timeout(initial_timeout, 0, timerfd, 0);
    }
    return timerfd;
}

/**
 * Helper to set a new trigger on timerfd in $start seconds
 */
void set_timeout(int sec, int nsec, int fd, int flag) {
    struct itimerspec timerValue = {{0}};

    timerValue.it_value.tv_sec = sec;
    timerValue.it_value.tv_nsec = nsec;
    timerValue.it_interval.tv_sec = 0;
    timerValue.it_interval.tv_nsec = 0;
    int r = timerfd_settime(fd, flag, &timerValue, NULL);
    if (r == -1) {
        ERROR("%s\n", strerror(errno));
    }
}

/* 
 * Start a module only if it is not disabled, it is not inited, and a proper init hook function has been setted.
 * Check if all deps modules have been started too.
 * If module has not a poll_cb (it is not waiting on poll), call poll_cb right now as it is fully started already.
 */
void init_modules(const enum modules module) {
    if (!modules[module].disabled && modules[module].init && !modules[module].inited) {
        if (modules[module].self->num_deps == modules[module].self->satisfied_deps) {
            modules[module].init();
            /* 
             * if module has been correctly inited, and it has no poll_cb,
             * start right now its dependent modules.
             */
            if (modules[module].inited && !modules[module].poll_cb) {
                poll_cb(module);
            }
        }
    }
}

void init_module(int fd, enum modules module, void (*cb)(void)) {
    if (fd == -1) {
        state.quit = 1;
        return;
    }

    main_p[module] = (struct pollfd) {
        .fd = fd,
        .events = POLLIN,
    };
    modules[module].poll_cb = cb;
    
    /* 
     * if fd==DONT_POLL_W_ERR, it means a not-critical error happened
     * while module was setting itself up, before calling init_module.
     * eg: geoclue2 support is enabled but geoclue2 could not be found.
     */
    if (fd != DONT_POLL_W_ERR) {
        modules[module].inited = 1;
        INFO("%s module started.\n", modules[module].self->name);
    } else {
        /* module should be disabled */
        disable_module(module); // disable this module and all of dependent module
    }
}

/*
 * Foreach dep, set self as dependent on that module
 */
void set_self_deps(struct self_t *self) {
    for (int i = 0; i < self->num_deps; i++) {
        struct module *m = &(modules[self->deps[i].dep]);
        m->num_dependent++;
        m->dependent_m = realloc(m->dependent_m, m->num_dependent * sizeof(*(m->dependent_m)));
        if (!m->dependent_m) {
            ERROR("Could not malloc.\n");
            break;
        }
        m->dependent_m[m->num_dependent - 1] = self;
    }
}

/*
 * Callback called when a module that is needed by some others
 * gets started: it increment satisfied_deps for each dependent modules
 * and tries to start them.
 * If these modules have still other unsatisfied deps, they won't start.
 */
static void started_cb(enum modules module) {
    for (int i = 0; i < modules[module].num_dependent; i++) {
        struct self_t *self = modules[module].dependent_m[i];
        if (!modules[self->idx].disabled) {
            self->satisfied_deps++;
            INFO("Trying to start %s module as its %s dependency was loaded.\n", self->name, modules[module].self->name);
            init_modules(self->idx);
        }
    }
}

/*
 * Calls correct cb function;
 * then, if there are modules depending on this module,
 * try to start them, only once.
 */
void poll_cb(const enum modules module) {    
    if (modules[module].poll_cb) {
        modules[module].poll_cb();
    }
    /* If module has deps, call start cb on them, to start them */
    if (modules[module].dependent_m) {
        started_cb(module);
        free(modules[module].dependent_m);
        modules[module].dependent_m = NULL;
    }
}

/*
 * Recursively disable a module and all of modules that require it (HARD dep).
 * If a module had a SOFT dep on "module", increment its 
 * satisfied_deps counter and try to init it.
 */
static void disable_module(const enum modules module) {
    modules[module].disabled = 1;
    INFO("Module %s disabled.\n", modules[module].self->name);
    for (int i = 0; i < modules[module].num_dependent; i++) {
        struct self_t *self = modules[module].dependent_m[i];
        for (int j = 0; j < self->num_deps; j++) {
            if (self->deps[j].dep == module) {
                if (self->deps[j].type == HARD) {
                    disable_module(self->idx); // recursive call
                } else {
                    self->satisfied_deps++;
                    init_modules(self->idx);
                }
                break;
            }
        }
    }
}

/*
 * Calls correct destroy function for each module
 */
void destroy_modules(const enum modules module) {
    /* Free list of dependent-on-this-module modules if it has still not been freed */
    if (modules[module].dependent_m) {
        free(modules[module].dependent_m);
    }
    if (modules[module].inited && modules[module].destroy) {
        modules[module].destroy();
        INFO("%s module destroyed.\n", modules[module].self->name);
    }
}
