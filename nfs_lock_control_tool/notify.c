#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <sys/param.h>
#include <sys/syslog.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <netdb.h>
#include <errno.h>
#include <grp.h>
#include <getopt.h>

#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>

#define NSM_PROGRAM	100024
#define NSM_VERSION	1
#define NSM_NOTIFY	6
#define MAXMSGSIZE	256

struct nsm_host {
	struct nsm_host *	next;
	char *			name;
	char *			path;
	struct sockaddr_storage	addr;
	struct addrinfo		*ai;
	time_t			last_used;
	time_t			send_next;
	unsigned int		timeout;
	unsigned int		retries;
	unsigned int		xid;
};

static int verbose;

#define v_printf	if (verbose) printf

static struct addrinfo *smn_lookup(const char *name)
{
	struct addrinfo	*ai, hint = {
#if HAVE_DECL_AI_ADDRCONFIG
		.ai_flags	= AI_ADDRCONFIG,
#endif	/* HAVE_DECL_AI_ADDRCONFIG */
		.ai_family	= AF_INET,
		.ai_protocol	= IPPROTO_UDP,
	};
	int error;

	if (!(error = getaddrinfo(name, NULL, &hint, &ai)))
		return ai;

	return NULL;
}

static void dump(void *data, int size)
{
	unsigned char *mem = data;
	int i;

	printf("Dumping %d bytes from 0x%p:\n", size, data);
	
	printf(" 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
	for (i = 0; i < size; i++) {
		printf("%02x ", mem[i]);
		if (i && !(i % 16))
			printf("\n");
	}
	printf("\n");
}

static void smn_set_port(struct sockaddr *sap, const unsigned short port)
{
	switch (sap->sa_family) {
	case AF_INET:
		((struct sockaddr_in *)sap)->sin_port = htons(port);
		break;
	case AF_INET6:
		((struct sockaddr_in6 *)sap)->sin6_port = htons(port);
		break;
	}
}

static unsigned short smn_get_port(const struct sockaddr *sap)
{
	switch (sap->sa_family) {
	case AF_INET:
		return ntohs(((struct sockaddr_in *)sap)->sin_port);
	case AF_INET6:
		return ntohs(((struct sockaddr_in6 *)sap)->sin6_port);
	}
	return 0;
}

static int recv_reply(int sock)
{
	uint32_t	msgbuf[MAXMSGSIZE], *p, *end;
	int		res;
	struct pollfd	pfd;
	long		wait = 10;

	pfd.fd = sock;
	pfd.events = POLLIN;

	v_printf("Waiting for server answer...\n");

	wait *= 1000;
	if (wait < 100)
		wait = 100;
	if (poll(&pfd, 1, wait) != 1) {
		fprintf(stderr, "Failed to receive the answer from server\n");
		return 0;
	}

	res = recv(sock, msgbuf, sizeof(msgbuf), 0);
	if (res < 0) {
		v_printf("Failed to receive server answer.\n");
		return -1;
	}

	v_printf("Received server answer. Checking...");
	
	p = msgbuf;
	end = p + (res >> 2);

	p++;	// Skip xid	
	if (*p++ != htonl(1)	/* must be REPLY */
	 || *p++ != htonl(0)	/* must be ACCEPTED */
	 || *p++ != htonl(0)	/* must be NULL verifier */
	 || *p++ != htonl(0)
	 || *p++ != htonl(0)) {	/* must be SUCCESS */
		fprintf(stderr, "Server returned error. Notify failed!\n");
		return -1;
	}

	if (p > end) {
		fprintf(stderr, "Server answer size is less, than it should be (%d instead of 24).\n", res);
		return -1;
	}


	return 0;
}

/*
 * Send notification to a single host
 */
static int notify_host(int sock, struct nsm_host *server, unsigned short server_port, char *client_name, int nstatd_state)
{
	struct sockaddr_storage address;
	struct sockaddr *dest = (struct sockaddr *)&address;
	socklen_t destlen = sizeof(address);
	static unsigned int	xid = 0;
	uint32_t		msgbuf[MAXMSGSIZE], *p;
	unsigned int		len;

	if (!xid)
		xid = getpid() + time(NULL);
	if (!server->xid)
		server->xid = xid++;

	if (server->ai == NULL) {
		server->ai = smn_lookup(server->name);
		if (server->ai == NULL) {
			fprintf(stderr, "DNS resolution of %s failed; "
				"retrying later\n", server->name);
			return -1;
		}
	}

	memset(msgbuf, 0, sizeof(msgbuf));

	p = msgbuf;
	*p++ = htonl(server->xid);
	*p++ = 0;
	*p++ = htonl(2);

	if (server->ai->ai_next == NULL)
		memcpy(&server->addr, server->ai->ai_addr,
					server->ai->ai_addrlen);
	else {
		struct addrinfo *first = server->ai;
		struct addrinfo **next = &server->ai;

		/* remove the first entry from the list */
		server->ai = first->ai_next;
		first->ai_next = NULL;
		/* find the end of the list */
		next = &first->ai_next;
		while ( *next )
			next = & (*next)->ai_next;
		/* put first entry at end */
		*next = first;
		memcpy(&server->addr, first->ai_addr,
					first->ai_addrlen);
	}

	smn_set_port((struct sockaddr *)&server->addr, server_port);

	memcpy(dest, &server->addr, destlen);
	/* Build an SM_NOTIFY packet */
	v_printf("Sending clearing locks message to server %s.\n", server->name);

	*p++ = htonl(NSM_PROGRAM);
	*p++ = htonl(NSM_VERSION);
	*p++ = htonl(NSM_NOTIFY);

	/* Auth and verf */
	*p++ = 0; *p++ = 0;
	*p++ = 0; *p++ = 0;

	/* state change */
	len = strlen(client_name);
	*p++ = htonl(len);
	memcpy(p, client_name, len);
	p += (len + 3) >> 2;
	*p++ = htonl(nstatd_state);

	len = (p - msgbuf) << 2;

	if (sendto(sock, msgbuf, len, 0, dest, destlen) < 0) {
		fprintf(stderr, "Sending Reboot Notification to "
			"'%s' failed: errno %d (%s)\n", server->name, errno, strerror(errno));
		return -2;
	}

	return recv_reply(sock);
}


int get_statd_port(char *host)
{
	struct sockaddr_in sin;
	struct pmaplist *prog_list;

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = inet_addr(host);
	sin.sin_port = htons(111);
	
	prog_list = pmap_getmaps(&sin);
	if (!prog_list) {
		fprintf(stderr, "Failed to connect to portmapper on %s\n", host);
		return 0;
	}

	do {
		static int iter = 0;
		struct pmap *m = &prog_list->pml_map;

		if ((m->pm_prog == NSM_PROGRAM) && (m->pm_prot == IPPROTO_UDP))
			return m->pm_port;
		iter++;
	} while(prog_list = prog_list->pml_next);
	
	fprintf(stderr, "rpc.statd not found on %s\n", host);
	return 0;
}

uint32_t get_statd_state(char *dir)
{
	char statd_path[1024];
	int fd;
	int result;
	uint32_t statd_state;

	if (strlen(dir) > 1000) {
		fprintf(stderr, "Path to rpc.statd database is too long (Max: 1000 symbols)\n");
		return 0;
	}
	strcpy(statd_path, dir);
	strcat(statd_path, "/state");
	fd = open(statd_path, O_RDONLY, 0644);
	if (fd < 0) {
		perror("open");
		return 0;
	}
	result = lseek(fd, 0, SEEK_SET);
	if (result < 0) {
		perror("lseek");
		return 0;
	}

	result = read(fd, &statd_state, 4);
	if (result < 0) {
		perror("read");
		return 0;	
	}
	return statd_state;
}

void help(char *name)
{
	printf("Usage: clear_nfs_locks -c client -s server [OPTIONS]\n\n", name);
	printf("\tclient                    Client IP or domain name, which locks have to be droped\n");
	printf("\tserver                    Server IP or domain name to drop locks on.\n");
	printf("\nOptions:\n");
	printf("\t-d=state_dir              Path to directory, where rpc.statd holds it's state data. Note: if it's not specified, you must specify 'statd_state' or '-f' option.\n");
	printf("\t                          Note: if it's not specified, you must specify 'statd_state' or '-f' option.\n\n");
	printf("\t-p=port                   Server's rpc.statd port. Requested using pormapper by default.\n\n");
	printf("\t-i=statd_state            rpc.statd state id. Obtained from 'state_dir/state' file if 'state_dir' is specified.\n\n");
	printf("\t-l=local_ip               Specify local ip address to work on.\n\n");
	printf("\t-v                        Be verbose: print work progress\n\n");
	printf("\t-f                        Force go over all possible rpc.statd state id values.\n");
	printf("\t                          Warning: Since program is unable to determine if the locks are dropped on server,\n");
	printf("\t                                   going over all possible values takes a lot of time.\n\n");
	printf("\nReport bugs to skinsbursky@parallels.com\n");
	return;
}

int main(int argc, char **argv)
{
	struct nsm_host host;
	struct sockaddr_storage address;
	struct sockaddr *local_addr = (struct sockaddr *)&address;
	int	sock = -1;
	struct addrinfo *ai;
	int result;
	static char *client;
	static char *state_dir;
	static char *server;
	static unsigned short port;
	static uint32_t statd_state;
	static char *local_address;
	static int forced;

	if (argc == 1) {
		help(argv[0]);
		return 0;
	}
	
	while ((result = getopt(argc, argv, "c:d:s:p:i:l:vfh")) != EOF) {
		switch (result) {
			case 'c':
				client = optarg;
				break;
			case 'd':
				state_dir = optarg;
				break;
			case 's':
				server = optarg;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'i':
				statd_state = atoi(optarg);
				break;
			case 'l':
				local_address = optarg;
				break;
			case 'v':
				verbose = 1;
				break;
			case 'f':
				forced = 1;
				break;
			case 'h':
				help(argv[0]);
				exit(0);
			default:
				fprintf(stderr, "%s: bad option '%c'\n", argv[0], result);
				help(argv[0]);
				exit(2);
		}
	}

	if (!client) {
		fprintf(stderr, "You must specity client.\n");
		help(argv[0]);
		exit(1);
	}

	if (!server) {
		fprintf(stderr, "You must specity server.\n");
		help(argv[0]);
		exit(1);
	}

	if (!state_dir && !statd_state && !forced) {
		fprintf(stderr, "You must specify at least 'state_dir' or 'server' and 'statd_state' values.\n");
		exit(1);
	}

	if (state_dir) {
		if ((statd_state = get_statd_state(state_dir)) <= 0) {
			fprintf(stderr, "Failed to get rpc.statd state value.\n");
			exit(1);
		}
		if (forced) {
			printf("Option '-f' ommited since 'state_dir' is specified\n");
			forced = 0;
		}
	}

	if (statd_state && forced) {
		printf("Option '-f' ommited since 'statd_state' is specified\n");
		forced = 0;
	}
	
	if (forced && verbose) {
		printf("Option '-v' ommited since '-f' is specified\n");
		verbose = 0;
	}

	if (!port) {
		if (!(port = get_statd_port(server)))
			exit(1);
	}

	if (local_address) {
		if (!(ai = smn_lookup(local_address))) {
			fprintf(stderr, "Not a valid hostname or address: \"%s\"\n",
				local_address);
			exit(1);
		}
	
		/* We know it's IPv4 at this point */
		memcpy(local_addr, ai->ai_addr, ai->ai_addrlen);
		freeaddrinfo(ai);
	}

	v_printf("Client          : '%s'\n", client);
	v_printf("Server          : '%s'\n", server);
	v_printf("Port            : %d\n", port);
	v_printf("prc.statd state : %d\n", statd_state);
	v_printf("Forced mode     : %s\n", (forced) ? "Yes" : "No");

	do {
		int retries = 10;
		struct servent *se;
		struct sockaddr_in *sin = (struct sockaddr_in *)local_addr;

		sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock < 0) {
			fprintf(stderr, "Failed to create RPC socket: %s\n",
				strerror(errno));
			exit(1);
		}
		fcntl(sock, F_SETFL, O_NONBLOCK);

		bindresvport(sock, sin);
		/* try to avoid known ports */
		if (se = getservbyport(sin->sin_port, "udp")) {
			retries--;
			close(sock);
		} else 
			break;
		if (!retries) {
			fprintf(stderr, "Failed to bind RPC socket: %s\n",
				stderror(errno));
			exit(1);
		}
	} while (1);

	memset (&host, 0, sizeof(struct nsm_host));
	host.name = server;

	if (!forced) {
		result = notify_host(sock, &host, port, client, statd_state);
	} else {
		int i;
		for (i = 1; i < UINT_MAX; i += 2)
			if ((result = notify_host(sock, &host, port, client, i)) < 0)
				break;
	}

	if (result < 0	)
		perror("Clearing NFS locks failed");
	else
		v_printf("Clearing NFS locks successfully completed.\n");

	close(sock);

	return result;
}
