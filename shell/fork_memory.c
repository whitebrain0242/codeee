#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
int global_var = 100;
int main()
{
    int local_var = 200;
    int *heap_var = malloc(sizeof(int));

    if (heap_var == NULL)
    {
        perror("malloc");
        return 1;
    }
    *heap_var = 300; // 记得释放

    printf("Before fork:\n");
    printf("  global_var = %d\n", global_var);
    printf("  local_var  = %d\n", local_var);
    printf("  heap_var   = %d\n", *heap_var);

    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        free(heap_var);
        return 1;
    }
    if (pid == 0)
    {
        global_var = 111;
        local_var = 222;
        *heap_var = 333;
        printf("\nChild changed values:\n");
        printf("  global_var = %d\n", global_var);
        printf("  local_var  = %d\n", local_var);
        printf("  heap_var   = %d\n", *heap_var);
        free(heap_var);
        _exit(0);
    }
    else
    {
        waitpid(pid, NULL, 0);
        printf("\nParent after child exits:\n");
        printf("  global_var = %d\n", global_var);
        printf("  local_var  = %d\n", local_var);
        printf("  heap_var   = %d\n", *heap_var);
        free(heap_var);
    }

    return 0;
}