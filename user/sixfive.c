#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void parsefile(char* fname)
{
    int fd = open(fname, 0);
    if (fd < 0) {
        fprintf(2, "sixfive: cannot open %s\n", fname);
        return;
    }

    char sep[] = " -\r\t\n./,";
    char numbuf[32];   // 存放当前连续数字字符串
    int pos = 0;       // numbuf当前长度
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c >= '0' && c <= '9') {
            // 是数字，存入缓冲区
            if (pos < sizeof(numbuf) - 1) {
                numbuf[pos++] = c;
            }
        }
        else {
            // 判断是不是分隔符
            if (strchr(sep, c) != 0) {
                if (pos > 0) {
                    numbuf[pos] = '\0'; // 构成C字符串
                    int val = atoi(numbuf);
                    if (val % 5 == 0 || val % 6 == 0) {
                        printf("%d\n", val);
                    }
                    pos = 0; // 清空数字缓存
                }
            }
            // 非数字、非分隔符直接忽略
        }
    }

    // 文件读到末尾，处理残留数字
    if (pos > 0) {
        numbuf[pos] = '\0';
        int val = atoi(numbuf);
        if (val % 5 == 0 || val % 6 == 0) {
            printf("%d\n", val);
        }
    }

    close(fd);
}

int
main(int argc, char* argv[])
{
    // 允许多个文件
    if (argc < 2) {
        fprintf(2, "usage: sixfive filename [filename...]\n");
        exit(1);
    }

    // 循环遍历所有传入文件
    for (int i = 1; i < argc; i++) {
        parsefile(argv[i]);
    }

    exit(0);
}
