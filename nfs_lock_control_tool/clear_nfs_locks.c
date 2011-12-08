#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <getopt.h>
#include <sys/poll.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>

#define NSM_PROGRAM		100024
#define NSM_VERSION		1
#define NSM_NOTIFY		6
#define MAX_MESSAGE_SIZE	256

#define MAGIC_NSM_STATE		1
#define SERVER_ANSWER_SIZE	24

struct server_info {
	char *			name;
	unsigned short		statd_port;
};

struct client_info {
	char		*name;
	int		name_lenght;
	uint32_t	statd_state;
};

static int verbose;

#define v_printf	if (verbose) printf

static struct addrinfo *host_lookup(const char *node)
{
	struct addrinfo	hints = {
		.ai_family	= AF_INET,
		.ai_protocol	= IPPROTO_UDP,
	};
	struct addrinfo	*result;

	if (!getaddrinfo(node, NULL, &hints, &result))
		return result;

	return NULL;
}

static int receive_server_answer(int sock)
{
	unsigned int	buffer[MAX_MESSAGE_SIZE];
	int		result;
	struct pollfd	pfd;
	long		wait_in_msec = 10000;

	pfd.fd = sock;
	pfd.events = POLLIN;

	v_printf("Waiting for server answer...\n");

	if ((poll(&pfd, 1, wait_in_msec) != 1) ||
		((result = recv(sock, buffer, sizeof(buffer), 0)) < 0)) {
		fprintf(stderr, "Failed to receive the answer from server\n");
		return -1;
	}

	v_printf("Received server answer. Checking...\n");

	if (result < SERVER_ANSWER_SIZE) {
		fprintf(stderr, "Server answer size is less, than it should be "
			       		"(%d instead of %d).\n", result, SERVER_ANSWER_SIZE);
		return -1;
	}
	
	if (buffer[1] != htonl(1) ||	/* Reply code */
	    buffer[2] != htonl(0) ||	/* Accepted code */
	    buffer[3] != htonl(0) ||
	    buffer[4] != htonl(0) ||
	    buffer[4] != htonl(0)) {	/* Success code */
		fprintf(stderr, "Server returned error. Notify failed!\n");
		return -1;
	}

	return 0;
}

/*
 * Send notification to a single host
 */
static int send_unlock_message(int sock, struct server_info *server, 
					struct client_info *client)
{
	struct addrinfo *ai;
	struct sockaddr_storage address;
	struct sockaddr *addr = (struct sockaddr *)&address;
	struct sockaddr_in *addr_in = (struct sockaddr_in *)&address;
	struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&address;
	uint32_t buffer[MAX_MESSAGE_SIZE], *p;
	unsigned pkt_size;

	ai = host_lookup(server->name);
	if (!ai) {
		fprintf(stderr, "DNS resolution of %s failed\n", server->name);
		return -1;
	}

	memcpy(&address, ai->ai_addr, ai->ai_addrlen);

	if (addr->sa_family == AF_INET)
		addr_in->sin_port = htons(server->statd_port);
	else
		addr_in6->sin6_port = htons(server->statd_port);

	/* Create SM_NOTIFY packet */
	memset(buffer, 0, sizeof(buffer));

	p = &buffer[2];
	*p++ = htonl(2);
	*p++ = htonl(NSM_PROGRAM);
	*p++ = htonl(NSM_VERSION);
	*p++ = htonl(NSM_NOTIFY);
	p += 4;

	*p++ = htonl(client->name_lenght);
	memcpy(p, client->name, client->name_lenght);
	p += (client->name_lenght + 3) >> 2;

	*p++ = htonl(client->statd_state);

	pkt_size = (p - buffer) << 2;

	v_printf("Sending clearing locks message to server %s with state %d...\n", 
							server->name, 
							client->statd_state);

	if (sendto(sock, buffer, pkt_size, 0, addr, sizeof(address)) < 0) {
		fprintf(stderr, "Sending clearing locks message to %s failed: %s",
					server->name, strerror(errno));
		return -2;
	}

	return 0;
}

static int nfs_clear_locks(int sock, struct server_info *server,
	       				struct client_info *client)
{
	int error;
	
	error = send_unlock_message(sock, server, client);
	if (!error)
		error = receive_server_answer(sock);

	return error;
}

static int clear_nfs_locks(int sock, struct server_info *server_info,
				char *client_name, uint32_t statd_state)
{
	struct client_info client_info;

	memset (&client_info, 0, sizeof(struct client_info));
	client_info.name = client_name;
	client_info.name_lenght = strlen(client_name);

	if (statd_state == MAGIC_NSM_STATE) {
		int result;
		/* Tricky hack. At least some versions of rpc.statd doesn't 
		 * drops locks, when receiving reboot counter, equal to 1, 
		 * if servers reboot counter for this client is equal to 3.
		 * So, we first try state equal to 3, and than magic state.
		 */
		client_info.statd_state = 3;
		result = nfs_clear_locks(sock, server_info, &client_info);
		if (result < 0)
		       return result;	
	}

	client_info.statd_state = statd_state;
	return nfs_clear_locks(sock, server_info, &client_info);
}

static int get_statd_port(char *host)
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

static void help(char *name)
{
	printf("Usage: clear_nfs_locks -c client_name -s server [OPTIONS]\n\n",
									name);
	printf("\tclient_name               Client domain name, which locks "
					    "have to be droped. Server uses "
					    "this name as an identifier.\n");
	printf("\tserver_name                Server domain name or IP to drop "
					    "locks on.\n");
	printf("\nOptions:\n");
	printf("\t-p port                   Server's rpc.statd port. Requested "
					    "using pormapper by default.\n\n");
	printf("\t-i statd_state              rpc.statd state id (reboot counter)."
		       			    " Used '1' (magic state) "
					    "by default.\n\n");
	printf("\t-l local_ip               Specify local ip address to "
					    "work on.\n\n");
	printf("\t-v                        Be verbose: print work progress\n\n");
	printf("\t-h                        This help.\n\n");
	printf("Report bugs to skinsbursky@parallels.com\n");
	return;
}

int main(int argc, char **argv)
{
	static char *client_name, *server_name;
	static unsigned short port;
	static char *local_address;
	uint32_t statd_state = MAGIC_NSM_STATE;
	struct server_info server_info;
	struct sockaddr_storage address;
	struct sockaddr *local_addr = (struct sockaddr *)&address;
	int sock, result, bind_retries = 10;

	if (argc == 1) {
		printf("Missed required options. Try '-h' to get help.\n");
		return 0;
	}
	
	while ((result = getopt(argc, argv, "c:s:p:i:l:vh")) != EOF) {
		switch (result) {
			case 'c':
				client_name = optarg;
				break;
			case 's':
				server_name = optarg;
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
			case 'h':
				help(argv[0]);
				exit(0);
			default:
				fprintf(stderr,
					"%s: bad option '%c'\n",
					argv[0], result);
				help(argv[0]);
				exit(2);
		}
	}

	if (!client_name) {
		fprintf(stderr, "You must specity client name.\n");
		help(argv[0]);
		exit(1);
	}

	if (!server_name) {
		fprintf(stderr, "You must specity server.\n");
		help(argv[0]);
		exit(1);
	}

	if (!port) {
		if (!(port = get_statd_port(server_name)))
			exit(1);
	}

	if (local_address) {
		struct addrinfo *ai;

		if (!(ai = host_lookup(local_address))) {
			fprintf(stderr,
				"Not a valid hostname or address: \"%s\"\n",
				local_address);
			exit(1);
		}
	
		/* We know it's IPv4 at this point */
		memcpy(local_addr, ai->ai_addr, ai->ai_addrlen);
		freeaddrinfo(ai);
	}

	v_printf("Client name     : '%s'\n", client_name);
	v_printf("Server name     : '%s'\n", server_name);
	v_printf("Port            : %d\n", port);
	v_printf("rpc.statd state : %u\n", statd_state);

	do {
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
		se = getservbyport(sin->sin_port, "udp");
		if (!se)
			break;

		close(sock);
	} while(bind_retries--);

	if (!bind_retries) {
		fprintf(stderr, "Failed to bind RPC socket: %s\n",
			strerror(errno));
		exit(1);
	}

	memset (&server_info, 0, sizeof(struct server_info));
	server_info.name = server_name;
	server_info.statd_port = port;

	result = clear_nfs_locks(sock, &server_info, client_name, statd_state);
	if (result < 0)
		perror("Clearing NFS locks failed.");
	else
		v_printf("Clearing NFS locks successfully completed.\n");

error:
	close(sock);
	return result;
}
