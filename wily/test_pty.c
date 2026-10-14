#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <wily.h>
#include <event.h>

int main() {
    int master, slave;
    pid_t pid;
    
    printf("Testing PTY creation...\n");
    
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        perror("openpty");
        return 1;
    }
    
    printf("PTY created: master=%d, slave=%d\n", master, slave);
    
    pid = fork();
    if (pid < 0) {
        perror("fork");
        close(master);
        close(slave);
        return 1;
    } else if (pid == 0) {
        // Child
        close(master);
        setsid();
        ioctl(slave, TIOCSCTTY, 1);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        execlp("sh", "sh", "-i", NULL);
        perror("execlp");
        _exit(1);
    } else {
        // Parent
        close(slave);
        printf("Forked shell: pid=%d, master fd=%d\n", pid, master);
        
        // Test estart
        unsigned long key = estart(0, master, 0);
        printf("estart returned: %lu\n", key);
        if (key == 0) {
            perror("estart failed");
            close(master);
            return 1;
        }
        
        // Clean up
        estop(key);
        close(master);
    }
    
    return 0;
}
