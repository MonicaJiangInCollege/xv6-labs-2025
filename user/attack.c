#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
 char *start = sbrk(0);

  for(int i = 0; i < 256; i++){
    if(sbrk(4096) == (char *)-1)
      break;
  }

  char *end = sbrk(0);

  for(char *p = start; p < end - 32; p++){

    if(memcmp(p, "This may help.", 14) == 0){

      char *secret = p + 16;

      write(1, secret, strlen(secret));
      write(1,"\n",1);

      exit(0);
    }
  }

  exit(1);
}
