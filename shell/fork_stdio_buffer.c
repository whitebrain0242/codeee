#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
int main(int argc, char *argv[])
{
    printf("HELLO!");
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return 1;
    }
    if (pid == 0)
    {
        if (argc >= 2 && strcmp(argv[1], "_exit") == 0)
        {
            _exit(0);
        }
        else
        {
            exit(0);
        }
    }
    wait(NULL);
    return 0;
}