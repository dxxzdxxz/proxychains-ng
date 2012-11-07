#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "allocator_thread.h"
#include "shm.h"
#include "debug.h"
#include "ip_type.h"
#include "mutex.h"

struct stringpool mem;
static char *at_dumpstring(char* s, size_t len) {
	PFUNC();
	return stringpool_add(&mem, s, len);
}


/* stuff for our internal translation table */

typedef struct {
	uint32_t hash;
	char* string;
} string_hash_tuple;

typedef struct {
	uint32_t counter;
	uint32_t capa;
	string_hash_tuple** list;
} internal_ip_lookup_table;

pthread_mutex_t internal_ips_lock;
internal_ip_lookup_table *internal_ips = NULL;
internal_ip_lookup_table internal_ips_buf;

uint32_t dalias_hash(char *s0) {
	unsigned char *s = (void *) s0;
	uint_fast32_t h = 0;
	while(*s) {
		h = 16 * h + *s++;
		h ^= h >> 24 & 0xf0;
	}
	return h & 0xfffffff;
}

uint32_t index_from_internal_ip(ip_type internalip) {
	PFUNC();
	ip_type tmp = internalip;
	uint32_t ret;
	ret = tmp.octet[3] + (tmp.octet[2] << 8) + (tmp.octet[1] << 16);
	ret -= 1;
	return ret;
}

char *string_from_internal_ip(ip_type internalip) {
	PFUNC();
	char *res = NULL;
	uint32_t index = index_from_internal_ip(internalip);
	if(index < internal_ips->counter)
		res = internal_ips->list[index]->string;
	return res;
}

extern unsigned int remote_dns_subnet;
ip_type make_internal_ip(uint32_t index) {
	ip_type ret;
	index++; // so we can start at .0.0.1
	if(index > 0xFFFFFF)
		return ip_type_invalid;
	ret.octet[0] = remote_dns_subnet & 0xFF;
	ret.octet[1] = (index & 0xFF0000) >> 16;
	ret.octet[2] = (index & 0xFF00) >> 8;
	ret.octet[3] = index & 0xFF;
	return ret;
}

static ip_type ip_from_internal_list(char* name, size_t len) {
	uint32_t hash = dalias_hash((char *) name);
	size_t i;
	ip_type res;
	void* new_mem;
	// see if we already have this dns entry saved.
	if(internal_ips->counter) {
		for(i = 0; i < internal_ips->counter; i++) {
			if(internal_ips->list[i]->hash == hash && !strcmp(name, internal_ips->list[i]->string)) {
				res = make_internal_ip(i);
				PDEBUG("got cached ip for %s\n", name);
				goto have_ip;
			}
		}
	}
	// grow list if needed.
	if(internal_ips->capa < internal_ips->counter + 1) {
		PDEBUG("realloc\n");
		new_mem = realloc(internal_ips->list, (internal_ips->capa + 16) * sizeof(void *));
		if(new_mem) {
			internal_ips->capa += 16;
			internal_ips->list = new_mem;
		} else {
	oom:
			PDEBUG("out of mem\n");
			goto err_plus_unlock;
		}
	}

	res = make_internal_ip(internal_ips->counter);
	if(res.as_int == ip_type_invalid.as_int)
		goto err_plus_unlock;

	string_hash_tuple tmp = { 0 };
	new_mem = at_dumpstring((char*) &tmp, sizeof(string_hash_tuple));
	if(!new_mem)
		goto oom;

	PDEBUG("creating new entry %d for ip of %s\n", (int) internal_ips->counter, name);

	internal_ips->list[internal_ips->counter] = new_mem;
	internal_ips->list[internal_ips->counter]->hash = hash;
	
	new_mem = at_dumpstring((char*) name, len + 1);
	
	if(!new_mem) {
		internal_ips->list[internal_ips->counter] = 0;
		goto oom;
	}
	internal_ips->list[internal_ips->counter]->string = new_mem;

	internal_ips->counter += 1;

	have_ip:

	return res;
	err_plus_unlock:
	
	PDEBUG("return err\n");
	return ip_type_invalid;
}

/* stuff for communication with the allocator thread */

enum at_msgtype {
	ATM_GETIP,
	ATM_GETNAME,
	ATM_EXIT,
};

enum at_direction {
	ATD_SERVER = 0,
	ATD_CLIENT,
	ATD_MAX,
};

struct at_msghdr {
	enum at_msgtype msgtype;
	size_t datalen;
};

static pthread_t allocator_thread;
static pthread_attr_t allocator_thread_attr;
static int req_pipefd[2];
static int resp_pipefd[2];

static int wait_data(int readfd) {
	PFUNC();
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(readfd, &fds);
	int ret;
	while((ret = select(readfd+1, &fds, NULL, NULL, NULL)) <= 0) {
		if(ret < 0) {
			perror("select2");
			return 0;
		}
	}
	return 1;
}

static int sendmessage(enum at_direction dir, struct at_msghdr *hdr, void* data) {
	static int* destfd[ATD_MAX] = { [ATD_SERVER] = &req_pipefd[1], [ATD_CLIENT] = &resp_pipefd[1] };
	int ret = write(*destfd[dir], hdr, sizeof *hdr) == sizeof *hdr;
	if(ret && hdr->datalen) {
		assert(hdr->datalen <= MSG_LEN_MAX);
		ret = write(*destfd[dir], data, hdr->datalen) == hdr->datalen;
	}
	return ret;
}

static int getmessage(enum at_direction dir, struct at_msghdr *hdr, void* data) {
	static int* readfd[ATD_MAX] = { [ATD_SERVER] = &req_pipefd[0], [ATD_CLIENT] = &resp_pipefd[0] };
	int ret;
	if((ret = wait_data(*readfd[dir]))) {
		ret = read(*readfd[dir], hdr, sizeof *hdr) == sizeof(*hdr);
		assert(hdr->datalen <= MSG_LEN_MAX);
		if(ret && hdr->datalen) {
			ret = read(*readfd[dir], data, hdr->datalen) == hdr->datalen;
		}
	}
	return ret;
}

static void* threadfunc(void* x) {
	(void) x;
	int ret;
	struct at_msghdr msg;
	union { 
		char host[MSG_LEN_MAX];
		ip_type ip;
	} readbuf;
	while((ret = getmessage(ATD_SERVER, &msg, &readbuf))) {
		switch(msg.msgtype) {
			case ATM_GETIP:
				/* client wants an ip for a DNS name. iterate our list and check if we have an existing entry.
					* if not, create a new one. */
				readbuf.ip = ip_from_internal_list(readbuf.host, msg.datalen - 1);
				msg.datalen = sizeof(ip_type);
				break;
			case ATM_GETNAME: {
				char *host = string_from_internal_ip(readbuf.ip);
				if(host) {
					size_t l = strlen(host);
					assert(l < MSG_LEN_MAX);
					memcpy(readbuf.host, host, l + 1);
					msg.datalen = l + 1;
				}
				break;
			}
			case ATM_EXIT:
				return 0;
			default:
				abort();
		}
		ret = sendmessage(ATD_CLIENT, &msg, &readbuf);
	}
	return 0;
}

/* API to access the internal ip mapping */

ip_type at_get_ip_for_host(char* host, size_t len) {
	ip_type readbuf;
	MUTEX_LOCK(&internal_ips_lock);
	if(len > MSG_LEN_MAX) goto inv;
	struct at_msghdr msg = {.msgtype = ATM_GETIP, .datalen = len + 1 };
	if(sendmessage(ATD_SERVER, &msg, host) &&
	   getmessage(ATD_CLIENT, &msg, &readbuf));
	else {
		inv:
		readbuf = ip_type_invalid;
	}
	MUTEX_UNLOCK(&internal_ips_lock);
	return readbuf;
}

size_t at_get_host_for_ip(ip_type ip, char* readbuf) {
	struct at_msghdr msg = {.msgtype = ATM_GETNAME, .datalen = sizeof(ip_type) };
	size_t res = 0;
	MUTEX_LOCK(&internal_ips_lock);
	if(sendmessage(ATD_SERVER, &msg, &ip) && getmessage(ATD_CLIENT, &msg, readbuf)) {
		if((ptrdiff_t) msg.datalen <= 0) res = 0;
		else res = msg.datalen - 1;
	}
	MUTEX_UNLOCK(&internal_ips_lock);
	return res;
}


static void initpipe(int* fds) {
	if(pipe(fds) == -1) {
		perror("pipe");
		exit(1);
	}
}

/* initialize with pointers to shared memory. these will
 * be used to place responses and arguments */
void at_init(void) {
	PFUNC();
	MUTEX_INIT(&internal_ips_lock);
	internal_ips = &internal_ips_buf;
	memset(internal_ips, 0, sizeof *internal_ips);
	initpipe(req_pipefd);
	initpipe(resp_pipefd);
	stringpool_init(&mem);
	pthread_attr_init(&allocator_thread_attr);
	pthread_attr_setstacksize(&allocator_thread_attr, 16 * 1024);
	pthread_create(&allocator_thread, &allocator_thread_attr, threadfunc, 0);
}

void at_close(void) {
	PFUNC();
	const int msg = ATM_EXIT;
	write(req_pipefd[1], &msg, sizeof(int));
	pthread_join(allocator_thread, NULL);
	close(req_pipefd[0]);
	close(req_pipefd[1]);
	close(resp_pipefd[0]);
	close(resp_pipefd[1]);
	pthread_attr_destroy(&allocator_thread_attr);
	MUTEX_DESTROY(&internal_ips_lock);
}