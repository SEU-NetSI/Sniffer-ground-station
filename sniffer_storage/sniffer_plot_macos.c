#include "sniffer_plot_macos.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int locate_script(char path[PATH_MAX]) {
    static const char *candidates[] = {
        "../analyze_sniffer_binary.py",
        "analyze_sniffer_binary.py",
    };
    for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        if (realpath(candidates[index], path) != NULL) {
            return 0;
        }
    }
    return -1;
}

static int build_default_output(const char *project_root,
                                const char *capture_path,
                                char output[PATH_MAX]) {
    const char *name = strrchr(capture_path, '/');
    name = name == NULL ? capture_path : name + 1;
    size_t name_length = strlen(name);
    if (name_length > 4 && strcmp(name + name_length - 4, ".bin") == 0) {
        name_length -= 4;
    }
    int written = snprintf(output, PATH_MAX, "%s/outputs_binary/%.*s",
                           project_root, (int)name_length, name);
    return written > 0 && written < PATH_MAX ? 0 : -1;
}

int sniffer_plot_macos_run(const char *capture_path, const char *output_dir,
                           double period_ms) {
    if (capture_path == NULL || period_ms <= 0.0) {
        return -1;
    }

    char script[PATH_MAX];
    if (locate_script(script) != 0) {
        fprintf(stderr, "Could not locate analyze_sniffer_binary.py\n");
        return -1;
    }
    char project_root[PATH_MAX];
    if (strlen(script) >= sizeof(project_root)) {
        return -1;
    }
    strcpy(project_root, script);
    char *separator = strrchr(project_root, '/');
    if (separator == NULL) {
        return -1;
    }
    *separator = '\0';

    char generated_output[PATH_MAX];
    if (output_dir == NULL) {
        if (build_default_output(project_root, capture_path,
                                 generated_output) != 0) {
            fprintf(stderr, "Could not derive plot output directory\n");
            return -1;
        }
        output_dir = generated_output;
    }

    char python[PATH_MAX];
    int python_length = snprintf(python, sizeof(python), "%s/.venv/bin/python",
                                 project_root);
    if (python_length <= 0 || python_length >= (int)sizeof(python) ||
        access(python, X_OK) != 0) {
        fprintf(stderr, "Could not execute %s/.venv/bin/python\n", project_root);
        return -1;
    }

    char period[64];
    snprintf(period, sizeof(period), "%.12g", period_ms);
    char *arguments[] = {
        python,
        script,
        (char *)capture_path,
        "--output",
        (char *)output_dir,
        "--period-ms",
        period,
        "--timestamp-bits",
        "40",
        NULL,
    };

    printf("Generating phase plot in %s\n", output_dir);
    fflush(stdout);
    pid_t child = 0;
    int response = posix_spawn(&child, python, NULL, NULL, arguments, environ);
    if (response != 0) {
        fprintf(stderr, "Could not start analyzer: %s\n", strerror(response));
        return -1;
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Analyzer exited unsuccessfully\n");
        return -1;
    }
    printf("Phase plot saved to %s/phase.png\n", output_dir);
    return 0;
}
