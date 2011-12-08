#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include "vzcalluser.h"
#include "vzerror.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>

#define EI_NIDENT	16
#define ELFMAG		"\177ELF"
#define OLFMAG		"\177OLF"

#define ELFCLASSNONE	0
#define ELFCLASS32	1
#define ELFCLASS64	2

#include <sys/syscall.h>

#ifdef __ia64__
#define __NR_fairsched_vcpus	1499
#define __NR_fairsched_chwt	1502
#define __NR_fairsched_rate	1504
#define __NR_setluid		1506
#define __NR_setublimit		1507
#define __NR_ioprio_set		1274
#elif __x86_64__
#define __NR_fairsched_vcpus	499
#define __NR_setluid		501
#define __NR_setublimit		502
#define __NR_fairsched_chwt	506
#define __NR_fairsched_rate	508
#define __NR_ioprio_set		251
#elif __powerpc__
#define __NR_fairsched_chwt	402
#define __NR_fairsched_rate	404
#define __NR_fairsched_vcpus	405
#define __NR_setluid		411
#define __NR_setublimit		412
#define __NR_ioprio_set		273
#elif defined(__i386__) || defined(__sparc__)
#define __NR_fairsched_chwt	502
#define __NR_fairsched_rate	504
#define __NR_fairsched_vcpus	505
#define __NR_setluid		511
#define __NR_setublimit		512
#ifdef __sparc__
#define __NR_ioprio_set		196
#else
#define __NR_ioprio_set		289
#endif
#else
#error "no syscall for this arch"
#endif

enum {elf_none, elf_32, elf_64};

#define ENVRETRY	3

struct elf_hdr_s {
	uint8_t ident[EI_NIDENT];
	uint16_t type;
	uint16_t machine;
};

static inline int check_elf_magic(const uint8_t *buf)
{
	if (memcmp(buf, ELFMAG, 4) &&
		memcmp(buf, OLFMAG, 4))
	{
		return -1;
	}
	return 0;
}

int get_arch_from_elf(const char *file)
{
	int fd, nbytes, class;
	struct stat st;
	struct elf_hdr_s elf_hdr;

	if (stat(file, &st))
		return -1;
	if (!S_ISREG(st.st_mode))
		return -1;
	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -1;
	nbytes = read(fd, (void *) &elf_hdr, sizeof(elf_hdr));
	close(fd);
	if (nbytes < (int)sizeof(elf_hdr))
		return -1;
	if (check_elf_magic(elf_hdr.ident))
		return -1;
	class = elf_hdr.ident[4];
	switch (class) {
	case ELFCLASS32:
		return elf_32;
	case ELFCLASS64:
		return elf_64;
	}
	return elf_none;
}

#ifdef  __x86_64__
static int set_personality(unsigned long mask)
{
	unsigned long per;

	per = personality(0xffffffff) | mask;
	printf("Set personality %#10.8lx", per);
	if (personality(per) == -1) {
		perror("Unable to set personality PER_LINUX32");
		return  -1;
	}
	return 0;
}

static int set_personality32()
{
	if (get_arch_from_elf("/sbin/init") != elf_32)
		return 0;
	return set_personality(PER_LINUX32);
}
#endif

int vz_env_create_ioctl(int vzfd, envid_t veid, int flags)
{
	struct vzctl_env_create env_create;
	int errcode;
	int retry = 0;

	memset(&env_create, 0, sizeof(env_create));
	env_create.veid = veid;
	env_create.flags = flags;
	do {
		if (retry)
			sleep(1);
		errcode = ioctl(vzfd, VZCTL_ENV_CREATE, &env_create);
	} while (errcode < 0 && errno == EBUSY && retry++ < ENVRETRY);
	if (errcode >= 0 && (flags & VE_ENTER)) {
		/* Clear supplementary group IDs */
		setgroups(0, NULL);
#ifdef  __x86_64__
		/* Set personality PER_LINUX32 for i386 based CTs */
		set_personality32();
#endif
	}
	return errcode;
}

static inline int setluid(uid_t uid)
{
	return syscall(__NR_setluid, uid);
}

int vz_setluid(envid_t veid)
{
	if (setluid(veid) == -1) {
		if (errno == ENOSYS)
			printf("Error: kernel does not support"
				" user resources. Please, rebuild with"
				" CONFIG_USER_RESOURCE=y");
		return VZ_SETLUID_ERROR;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int veid, vzfd, ret;
	int qwe = 10;
	int cld_pid;
	
	if (argc == 1) {
		printf("Specify container ID\n");
		return -1;
	}

	veid = atoi(argv[1]);

	if ((vzfd = open("/dev/vzctl", O_RDWR)) < 0) {
		perror("open vzctl");
		return -1;
	}

//	printf("Press any to create child and proceed...\n");

	if (cld_pid = fork()) {
		int status;
		printf("Waiting for child with pid %d.\n", cld_pid);
		wait(&status);
		printf("Child terminated.\n");
	} else {
		int cld_cnt_pid, cnt_status;

		printf("Entering container %d...\n", veid);
		if ((ret = vz_setluid(veid)))
			return ret;

		ret = vz_env_create_ioctl(vzfd, veid, VE_ENTER);
		if (ret < 0) {
			printf("Failed to enter container %d\n", veid);
			if (errno == ESRCH)
				ret = VZ_VE_NOT_RUNNING;
			else
				ret = VZ_ENVCREATE_ERROR;
			goto env_err;
		}

		printf("Successfully entered container %d.\n", veid);

		printf("Creating child inside container...\n");
		if (cld_cnt_pid = fork()) {
/*			while(qwe--) {	
				printf("Sleeping for 1 second...\n");
				sleep(1);
				printf("%d seconds left...\n", qwe);
			}
*/			printf("Child: Waiting for child with pid %d created inside container...\n", cld_cnt_pid);
			wait(&cnt_status);
			printf("Child: Child created inside container - terminated.\n");
		} else {
			int i = 0;
			printf("Child inside continer created successfully. Sleep for 1 sec.\n");
			if (daemon(0, 0) == -1) {
				perror("daemonize failed:");
				return -1;
			}
			while(i++ < 10) {
				printf("Sleeping for 1 second...\n");
				sleep(1);
			}
			return 0;
		}
	}
	return 0;
env_err:
	return -1;	
}
