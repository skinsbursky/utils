#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

int main(int argc, char **argv) 
{
	int fd;
	char *filename;
	struct flock fl;
	int sleeptime = 0;
	time_t start;

	if (argc < 2) {
		printf("Missing file name arg\n");
		return 1;
	}
	
	filename = argv[1];
	fd = open(filename, O_RDWR|O_CREAT,0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (argc == 3) {
		sleeptime = atoi(argv[2]);
	}

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	printf("Requesting fcntl lock of %s...\n", filename);
	start = time(0);
	if( fcntl (fd, F_SETLKW, &fl) < 0 ) {
		printf("Error: '%s' (lock request elapsed time %d secs)\n", strerror(errno), time(0) - start);
		return 1;
	}
	printf("fcntl lock %s : OK (time to lock %d secs)\n", filename, time(0) - start);
	printf("Press any key to unlock file...\n");
	getchar();
/*	printf("Sleeping for %d secs...\n", sleeptime);
	sleep(sleeptime);
*/
	fl.l_type = F_UNLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0;
	printf("Unlocking...\n");
	start = time(0);
	if( fcntl (fd, F_SETLK, &fl) < 0 ) {
		printf("Error (unlock elapsed time %d secs): ", time(0) - start);
		perror("fcntl unlock");
		return 1;
	}
	printf("Unlocked (time to unlock %d secs)\n", time(0) - start);
} 
