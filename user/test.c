#include "../kernel/param.h"
#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "../user/user.h"
#include "../kernel/fs.h"
#include "../kernel/fcntl.h"
#include "../kernel/syscall.h"
#include "../kernel/memlayout.h"
#include "../kernel/riscv.h"


int stack[1000];

int *sp;

#define push(sp, n) (*((sp)++) = (n))
#define pop(sp) (*--(sp))

void testCAS() {
    int forks = 5;
    for (int i = 0; i < forks; i++) {
        fork();
    }
    printf("PID: %d\n", getpid());
}
int
main(int argc, char *argv[])
{
    testCAS();
    // sp = stack; /* initialize */
    // for (int i = 0; i < 2; i++) {
    //    push(sp,fork());
    // }
    // sleep(10);

    // for (int i = 0; i < 10; i++)
    //     printf("%d, ", pop(sp));

    exit(0);
}