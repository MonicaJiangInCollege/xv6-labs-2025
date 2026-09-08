#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char* argv[])
{
    if (argc != 2) {
        printf("Usage: sleep ticks\n");
        exit(1);
    }
    int tick_num = atoi(argv[1]);
    pause(tick_num);
    exit(0);
}

