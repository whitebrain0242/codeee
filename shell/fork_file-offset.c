#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
int main(void){
    int fd=open("share-offset.txt",O_CREAT | O_TRUNC | O_RDWR, 0644);
    if(fd==-1){
        perror("open");
        return 1;
    }
    write(fd,"PARENT-BEFORE\n",14);
     printf("Parent before fork: file offset = %lld\n",
           (long long)lseek(fd, 0, SEEK_CUR));
    pid_t pid=fork();
    if(pid==-1){
        perror("fork");
        return 1;
    }
    if(pid==0){
        printf("Child starts: file offset = %lld\n",
               (long long)lseek(fd, 0, SEEK_CUR));
        write(fd, "CHILD-WRITE\n", 12);
        printf("Child after write: file offset = %lld\n",(long long)lseek(fd, 0, SEEK_CUR));
        close(fd);//记得关闭
        _exit(0);
    }else{
        waitpid(pid,NULL,0);
        printf("Parent after child exits: file offset = %lld\n",
               (long long)lseek(fd, 0, SEEK_CUR));
         write(fd, "PARENT-AFTER\n", 13);
         close(fd);
    }
    printf("\nNow run:\n");
    printf("  cat shared_offset.txt\n");
    return 0;
}