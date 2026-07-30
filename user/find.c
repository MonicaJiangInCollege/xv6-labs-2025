#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "kernel/param.h"
#include "user/user.h"

char *exec_argv[MAXARG];

void
find(char* path, char* target)
{
    int fd;
    struct stat st;
    struct dirent de;
    char buf[512];
    char* p;

    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (st.type != T_DIR) {
        close(fd);
        return;
    }

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0)
            continue;

        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';
        strcpy(p, de.name);

        if (strcmp(de.name, target) == 0) {
            if (exec_argv[0] == 0) {
                printf("%s\n", buf);
            } else {
                int pid = fork();
                if (pid == 0) {
                    char *child_argv[MAXARG];
                    int idx = 0;
                    for (int i = 0; exec_argv[i] != 0; i++) {
                        child_argv[idx++] = exec_argv[i];
                    }
                    child_argv[idx++] = buf;
                    child_argv[idx] = 0;

                    exec(child_argv[0], child_argv);
                    fprintf(2, "find: exec failed\n");
                    exit(1);
                } else {
                    wait(0);
                }
            }
        }

        if (stat(buf, &st) < 0)
            continue;

        if (st.type == T_DIR) {
            find(buf, target);
        }
    }
    close(fd);
}

int
main(int argc, char* argv[])
{
    memset(exec_argv, 0, sizeof(exec_argv));

    if (argc == 3) {
        find(argv[1], argv[2]);
        exit(0);
    }

    if (argc >= 5 && strcmp(argv[3], "-exec") == 0) {
        int pos = 0;
        for (int i = 4; i < argc; i++) {
            exec_argv[pos++] = argv[i];
        }
        exec_argv[pos] = 0;
        find(argv[1], argv[2]);
        exit(0);
    }

    fprintf(2, "usage:\n");
    fprintf(2, "  find dir filename\n");
    fprintf(2, "  find dir filename -exec cmd [args...]\n");
    exit(1);
}
