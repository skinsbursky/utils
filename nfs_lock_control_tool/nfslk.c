#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <rpc/rpc.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_clnt.h>

#if 0
struct mapping {
    unsigned int prog;
    unsigned int vers;
    unsigned int prot;
    unsigned int port;
};

const IPPROTO_TCP = 6;      /* protocol number for TCP/IP */
const IPPROTO_UDP = 17;     /* protocol number for UDP/IP */

struct pmaplist {
    mapping map;
    pmaplist *next;
};
#endif

#define RPC_BIND				100000

#define PMAP_NFSLOCKD				100021
#define PMAP_NFSLOCKD_VER			3

int main (int argc, char **argv)
{
	char *server = "10.30.20.35";
	struct sockaddr_in mount_server_addr;
	struct pmaplist *prog_list;
	struct pmap *m = &prog_list->pml_map;
	
	if (argc > 1)
		server = argv[1];

	mount_server_addr.sin_family = AF_INET;
	mount_server_addr.sin_addr.s_addr = inet_addr(server);
	mount_server_addr.sin_port = htons(111);
	
	prog_list = pmap_getmaps(&mount_server_addr);
	
	do {
		static int iter = 0;
		m = &prog_list->pml_map;
	
		if (m->pm_prog == RPC_BIND) {
			printf(" ----- Iteration %d -----\n", iter);
			printf("prog: 0x%x (%d)\n", m->pm_prog, m->pm_prog);
			printf("vers: 0x%x (%d)\n", m->pm_vers, m->pm_vers);
			printf("prot: 0x%x (%d)\n", m->pm_prot, m->pm_prot);
			printf("port: 0x%x (%d)\n", m->pm_port, m->pm_port);
		}
	
//		if ((m->pm_prog == PMAP_NFSLOCKD) && (m->pm_vers == PMAP_NFSLOCKD_VER))
//			break;
		m = NULL;
		iter++;
	} while(prog_list = prog_list->pml_next);
        
	if (m == NULL) {
		printf("Failed to find nfs.lockd version %d on %s\n", PMAP_NFSLOCKD_VER, server);
		return -1;
	}
	
	printf("Found nfs.lockd version %d on %s\n", PMAP_NFSLOCKD_VER, server);
	
	return 0;
}
