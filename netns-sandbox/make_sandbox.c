#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <printf.h>

int var = 0;

int exec_bash(void *data)
{
	char *argv[4];
	pid_t pid;
	char str_pid[16];

	memset(argv, 0, sizeof(argv));
	pid = getpid();
	sprintf(str_pid, "%d", pid);
	argv[0] = "test_bash";
	argv[1] = "/root/enter-sandbox";

	printf("Child is going to exec /bin/bash %s\n", argv[1]);
	execve("/bin/bash", argv, NULL);

	printf("Fuck! execve failed!\n");
}

int main(int argc, char **argv)
{
	void *child_stack;
	int child_pid, res, child_status;
	char cmd[128];

	if (system("echo 1 > /proc/sys/net/ipv4/conf/eth0/proxy_arp") < 0) return -1;
	if (system("echo 1 > /proc/sys/net/ipv4/conf/eth0/forwarding") < 0) return -1;
	if (system("ip link add type veth") < 0) return -1;
	if (system("ip l s veth0 up") < 0) return -1;
	if (system("brctl addif br0 veth0") < 0) return -1;

	child_stack = malloc(16384);
	if (!child_stack) {
		printf("Failed to alloc child stack\n");
		return -1;
	}

//	child_pid = clone(exec_bash, child_stack + 16384, CLONE_NEWNET |  SIGCHLD , NULL);
//	child_pid = clone(exec_bash, child_stack + 16384, CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWNS | SIGCHLD , NULL);
	child_pid = clone(exec_bash, child_stack + 16384, CLONE_NEWPID | CLONE_NEWNET | SIGCHLD , NULL);
	printf("Child pid: %d\n", child_pid);
	if (child_pid == -1) {
		printf("Failed to clone child: %d\n", errno);
		return -1;
	}

	printf("Pushing veth1 to %d\n", child_pid);
	sprintf(cmd, "ip l s veth1 netns %d", child_pid);
	system(cmd);

	res = waitpid(child_pid, &child_status, 0);
	if (res != child_pid) {
		printf("WTF?!: res = %d\n", res);
		return -1;
	}

	printf("Child exited with: %d\n", child_status);

	return 0;
}
