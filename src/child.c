#define _BSD_SOURCE
#define _POSIX_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mount.h>

#include "runner.h"
#include "child.h"
#include "logger.h"

#include "killer.h"


void close_file(FILE *fp, ...) {
    va_list args;
    va_start(args, fp);

    if (fp != NULL) {
        fclose(fp);
    }

    va_end(args);
}


int child_process(void *args) {
    FILE *log_fp = ((child_args *) args)->log_fp;
    struct config *_config = ((child_args *) args)->_config;

    // set memory limit
    if (_config->max_memory != UNLIMITED) {
        struct rlimit max_memory;
        max_memory.rlim_cur = max_memory.rlim_max = (rlim_t) (_config->max_memory) * 2;
        if (setrlimit(RLIMIT_AS, &max_memory) != 0) {
            CHILD_ERROR_EXIT(SETRLIMIT_FAILED);
        }
    }

    // set cpu time limit (in seconds)
    if (_config->max_cpu_time != UNLIMITED) {
        struct rlimit max_cpu_time;
        max_cpu_time.rlim_cur = max_cpu_time.rlim_max = (rlim_t) ((_config->max_cpu_time + 1000) / 1000);
        if (setrlimit(RLIMIT_CPU, &max_cpu_time) != 0) {
            CHILD_ERROR_EXIT(SETRLIMIT_FAILED);
        }
    }

    // set max process number limit
    if (_config->max_process_number != UNLIMITED) {
        struct rlimit max_process_number;
        max_process_number.rlim_cur = max_process_number.rlim_max = (rlim_t) _config->max_process_number;
        if (setrlimit(RLIMIT_NPROC, &max_process_number) != 0) {
            CHILD_ERROR_EXIT(SETRLIMIT_FAILED);
        }
    }

    // set max output size limit
    if (_config->max_output_size != UNLIMITED) {
        struct rlimit max_output_size;
        max_output_size.rlim_cur = max_output_size.rlim_max = (rlim_t ) _config->max_output_size;
        if (setrlimit(RLIMIT_FSIZE, &max_output_size) != 0) {
            CHILD_ERROR_EXIT(SETRLIMIT_FAILED);
        }
    }

    FILE *input_file = NULL, *output_file = NULL, *error_file = NULL;
    if (_config->input_path != NULL) {
        input_file = fopen(_config->input_path, "r");
        if (input_file == NULL) {
            CHILD_ERROR_EXIT(DUP2_FAILED);
        }
        // redirect file -> stdin
        // On success, these system calls return the new descriptor.
        // On error, -1 is returned, and errno is set appropriately.
        if (dup2(fileno(input_file), fileno(stdin)) == -1) {
            // todo log
            close_file(input_file);
            CHILD_ERROR_EXIT(DUP2_FAILED);
        }
    }

    if (_config->output_path != NULL) {
        output_file = fopen(_config->output_path, "w");
        if (output_file == NULL) {
            close_file(input_file);
            CHILD_ERROR_EXIT(DUP2_FAILED);
        }
        // redirect stdout -> file
        if (dup2(fileno(output_file), fileno(stdout)) == -1) {
            close_file(input_file, output_file);
            CHILD_ERROR_EXIT(DUP2_FAILED);
        }
    }

    if (_config->error_path != NULL) {
        // if outfile and error_file is the same path, we use the same file pointer
        if (strcmp(_config->output_path, _config->error_path) == 0) {
            error_file = output_file;
        }
        else {
            error_file = fopen(_config->error_path, "w");
            if (error_file == NULL) {
                // todo log
                close_file(input_file, output_file);
                CHILD_ERROR_EXIT(DUP2_FAILED);
            }
        }
        // redirect stderr -> file
        if (dup2(fileno(error_file), fileno(stderr)) == -1) {
            // todo log
            close_file(input_file, output_file, error_file);
            CHILD_ERROR_EXIT(DUP2_FAILED);
        }
    }

    // set gid
    gid_t group_list[] = {_config->gid};
    if (_config->gid != -1 && (setgid(_config->gid) == -1 || setgroups(sizeof(group_list) / sizeof(gid_t), group_list) == -1)) {
        CHILD_ERROR_EXIT(SETUID_FAILED);
    }

    // set uid
    if (_config->uid != -1 && setuid(_config->uid) == -1) {
        CHILD_ERROR_EXIT(SETUID_FAILED);
    }

    // load seccomp so
    if (_config->seccomp_rule_so_path != NULL) {
        void *handler = dlopen(_config->seccomp_rule_so_path, RTLD_LAZY);
        int (*load_seccomp)(void *, struct config *);

        if (!handler) {
            LOG_FATAL(log_fp, "seccomp failed");
            CHILD_ERROR_EXIT(LOAD_SECCOMP_FAILED);
        }
        load_seccomp = dlsym(handler, "load_seccomp");
        if (load_seccomp(handler, _config) != 0) {
            CHILD_ERROR_EXIT(LOAD_SECCOMP_FAILED);
        }
    }

    execve(_config->exe_path, _config->args, _config->env);
    CHILD_ERROR_EXIT(EXECVE_FAILED);
    return 0;
}