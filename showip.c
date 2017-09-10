#define _GNU_SOURCE     /* To get defns of NI_MAXSERV and NI_MAXHOST */
/* gai_strerror */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <ifaddrs.h>

/* IFF_LOOPBACK */
#include <sys/ioctl.h>
#include <net/if.h>

#include <stdio.h>  /* printing */
#include <stdlib.h> /* exit/EXIT_* */
#include <string.h> /* strncmp */
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>

#define ADDRSIZE 32
#define MAXLEN 256

enum ARGFLAGS {
	IPV4 = 1<<0,
	IPV6 = 1<<1,
	GUA6 = 1<<2,
	LLv6 = 1<<3,
	ULA6 = 1<<4,
	TMP6 = 1<<5,
	NTMP = 1<<6,
};

struct opts {
	int flags;
	/* Windows interface names can be up to 256. 
	 * Linux differs between 16 and 256.
	 */
	char interface[256];
};

static void usage()
{
	fprintf(stderr, "Usage: showip [-46gltTu] [interface]\n");

	exit(EXIT_FAILURE);
}

/* Pretty naïve arg parser */
/* struct opts has to be freed */
static struct opts *parse_flags(int argc, const char **argv)
{
	struct opts *options = malloc(sizeof *options);
	*options = (struct opts) {0, {'\0'}};

	if (argc < 2)
		return options;

	for (int i=1; i < argc; i++) {
		if (*argv[i] == '-') {
			for (const char *ch = argv[i]+1; *ch; ch++) {
				switch (*ch) {
					case '4': options->flags |= IPV4; break;
					case '6': options->flags |= IPV6; break;
					case 'g': options->flags |= GUA6; break;
					case 'l': options->flags |= LLv6; break;
					case 't': options->flags |= TMP6; break;
					case 'T': options->flags |= NTMP; break;
					case 'u': options->flags |= ULA6; break;
					default: usage();
				}
			}
		} else {
			/* strncpy doesn't always terminate with '\0'... */
			(void) strncpy(options->interface, argv[i], sizeof options->interface - 1);
			options->interface[sizeof options->interface -1] = '\0';
		}
	}

	return options;
}

static char *reduce_v6(char *addr)
{
	char *ret = calloc(ADDRSIZE*2, sizeof *ret);
	char *ende = NULL;
	bool leading = true;
	size_t setter = 0;
	int maxZeroSeq = 0;
	int zeroSeqCount = 0;

	/* Special case of first quartet */
	if (addr[0] != '0') {
		ret[setter++] = addr[0];
		leading=false;
	}

	/* Iterate over string stripping leading zeros 
	 * and calculating maximum zero sequence */
	for (size_t s=1,r=s%4; s < ADDRSIZE; s++,r=s%4) {
		/* Separate each quartet with a ``:'' */
		if (r == 0) {
			ret[setter++] = ':';
			leading=true;
		}

		/* strip at max 3 leading zeros. Else update Zerocounter */
		if (leading && addr[s] == '0') {
			if (r != 3) {
				continue;
			} else if (++zeroSeqCount > maxZeroSeq) {
				maxZeroSeq = zeroSeqCount;
				ende = ret+setter+2;
			}
		}

		/* Reset Zerocounter as necessary */
		leading=false;
		if (addr[s] != '0') {
			zeroSeqCount=0;
		}

		/* Copy chars */
		ret[setter++] = addr[s];
	}

	/* Squash multiple zero quartets if needed */
	if (maxZeroSeq > 0) {
		/* 2x as 0:0:... */
		char *start = ende-maxZeroSeq*2;
		if (start == ret)
			*start++ = ':';

		/* Copy chars over to strip 0:0:0... */
		for (*start++ = ':'; *(ende-1); *(start++) = *(ende++));
	}

	return ret;
}

static char **parse_proc()
{
	char **ret = malloc(MAXLEN * sizeof(char *));
	size_t size = 0;
	ssize_t read = 0;
	char *lineptr = NULL;
	char addr[ADDRSIZE];

	FILE *file = fopen("/proc/net/if_inet6", "r");
	if (file == NULL) {
		perror("fopen");
		exit(EXIT_FAILURE);
	}

	/* Read each line, split(' ') and add temporary addresses to list */
	size_t tmpNo=0;
	while ((read = getline(&lineptr, &size, file)) != -1) {
		for (int i=0,token=0; i<read; i++) {
			/* Buffer address */
			if (i < ADDRSIZE) {
				addr[i] = lineptr[i];
			} else if (isspace((int) lineptr[i])) {
				token++;

				if (token == 4) {
					addr[ADDRSIZE] = '\0';
					if (lineptr[i+2] != '1')
						break;

					/* Found temporary address, convert and add to list */
					ret[tmpNo++] = reduce_v6(addr);

					break;
				}
			}		
		}
	}

	/* Cleanup */
	free(lineptr);
	fclose(file);

	/* Terminate list */
	ret[tmpNo] = NULL;
	return ret;
}


static bool containsAddr(char *needle, char **haystack)
{
	for (; *haystack; haystack++)
		if (strcmp(needle, *haystack) == 0)
			return true;

	return false;
}

static void print_filtered(const struct ifaddrs *ifa, struct opts *options)
{
	int family, err;
	int flags = options->flags;
	char host[NI_MAXHOST];
	char **tmps = NULL;

	if (options->flags & TMP6 || options->flags & NTMP) {
		tmps = parse_proc();
	}

	/* Walk through linked list, maintaining head pointer so we
	   can free list later */
	for (; ifa != NULL; ifa = ifa->ifa_next) {
		/* Filter interfaces */
		if (
				(ifa->ifa_addr == NULL) || 
				(ifa->ifa_flags & IFF_LOOPBACK) ||
				(strncmp(ifa->ifa_name, "lo", 3) == 0) ||
				(options->interface[0] != '\0' && 
				 strncmp(ifa->ifa_name, options->interface, sizeof options->interface) != 0)
		   )
			continue;

		/* Only recognize IPv(4|6) */
		family = ifa->ifa_addr->sa_family;
		if (family == AF_INET || family == AF_INET6) {
			/* Get human readable address */
			err = getnameinfo(ifa->ifa_addr,
					(family == AF_INET) ? sizeof(struct sockaddr_in) 
					: sizeof(struct sockaddr_in6),
					host, NI_MAXHOST,
					NULL, 0, NI_NUMERICHOST);
			if (err != 0) {
				printf("getnameinfo() failed: %s\n", gai_strerror(err));
				exit(EXIT_FAILURE);
			}

			/* Filter according to options */
			if ((flags & NTMP) && containsAddr(host, tmps))
				continue;

			if (
					(flags == 0) ||
					(flags == NTMP) ||
					((flags & IPV4) && family == AF_INET) ||
					/* -6 equals -gul */
					((flags & IPV6) && family == AF_INET6) ||
					((flags & LLv6) && strncmp(host, "fe80", 4) == 0) ||
					((flags & TMP6) && containsAddr(host, tmps)) ||
					((flags & ULA6) && strncmp(host, "fd", 2) == 0) ||
					((flags & IPV6) && family == AF_INET6) ||
					/* Currently GUA is in the range of 2000::/3 2000... - 3fff... */
					((flags & GUA6) && (*host == '2' || *host == '3'))
			   ) {
				printf("%s\n", host);
			}
		}	
	}

	if (tmps)
		for (char **t = tmps; *t; t++)
			free(*t);
	free(tmps);
}

int main(int argc, const char **argv)
{
	struct ifaddrs *ifaddr;
	struct opts *opts = parse_flags(argc, argv);

	if (getifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		exit(EXIT_FAILURE);
	}

	print_filtered(ifaddr, opts);

	freeifaddrs(ifaddr);
	free(opts);
	exit(EXIT_SUCCESS);
}
