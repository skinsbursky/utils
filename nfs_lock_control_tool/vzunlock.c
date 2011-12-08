#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <linux/ioctl.h>

struct vzctl_nfs_unlock {
	char *srv_name;
	char *cln_name;
};

#define VZCTLTYPE '.'
#define VZCTL_NFS_UNLOCK	_IOW(VZCTLTYPE, 15,			\
					struct vzctl_nfs_unlock)

int main(int argc, char **argv) 
{
	int fd, tfd;
	char *filename;
	struct flock fl;
	int sleeptime = 0;
	time_t start;
	char *file = "/dev/vzctl";
	long result;
	struct vzctl_nfs_unlock s;

	if (argc < 3) {
		printf("Missing server host name or client hostname arg\n");
		return 1;
	}
	
	fd = open(file, O_RDWR, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	
	printf("Server name   : %s\n", argv[1]);
	printf("Client name   : %s\n", argv[2]);

	s.srv_name = argv[1];
	s.cln_name = argv[2];
	if(ioctl(fd, VZCTL_NFS_UNLOCK, &s)) {
		perror("ioctl failed");
		return 1;
	}

	/* ----------------------------- */
	return 0;
} 
