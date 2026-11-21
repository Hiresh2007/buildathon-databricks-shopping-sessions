#define _GNU_SOURCE
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    char path[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path));
    if (length < 0) {
        perror("readlink");
        return 1;
    }
    if ((size_t)length >= sizeof(path))
        return 1;
    path[length] = '\0';
    char *name = strrchr(path, '/');
    if (name == NULL)
        return 1;
    ++name;
    const char *target = __builtin_cpu_supports("x86-64-v3") ? "sessions-v3" : "sessions-generic";
    if (strlen(target) >= sizeof(path) - (size_t)(name - path))
        return 1;
    strcpy(name, target);
    execv(path, argv);
    perror(path);
    return 1;
}
