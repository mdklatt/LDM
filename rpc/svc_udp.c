/* @(#)svc_udp.c	2.2 88/07/29 4.0 RPCSRC */
/*
 * Sun RPC is a product of Sun Microsystems, Inc. and is provided for
 * unrestricted use provided that this legend is included on all tape
 * media and as a part of the software program in whole or part.  Users
 * may copy or modify Sun RPC without charge, but are not authorized
 * to license or distribute it to anyone else except as part of a product or
 * program developed by the user.
 * 
 * SUN RPC IS PROVIDED AS IS WITH NO WARRANTIES OF ANY KIND INCLUDING THE
 * WARRANTIES OF DESIGN, MERCHANTIBILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, OR ARISING FROM A COURSE OF DEALING, USAGE OR TRADE PRACTICE.
 * 
 * Sun RPC is provided with no support and without any obligation on the
 * part of Sun Microsystems, Inc. to assist in its use, correction,
 * modification or enhancement.
 * 
 * SUN MICROSYSTEMS, INC. SHALL HAVE NO LIABILITY WITH RESPECT TO THE
 * INFRINGEMENT OF COPYRIGHTS, TRADE SECRETS OR ANY PATENTS BY SUN RPC
 * OR ANY PART THEREOF.
 * 
 * In no event will Sun Microsystems, Inc. be liable for any lost revenue
 * or profits or other special, indirect and consequential damages, even if
 * Sun has been advised of the possibility of such damages.
 * 
 * Sun Microsystems, Inc.
 * 2550 Garcia Avenue
 * Mountain View, California  94043
 */
#if !defined(lint) && defined(SCCSIDS)
static char sccsid[] = "@(#)svc_udp.c 1.24 87/08/11 Copyr 1984 Sun Micro";
#endif

/*
 * svc_udp.c,
 * Server side for UDP/IP based RPC.  (Does some caching in the hopes of
 * achieving execute-at-most-once semantics.)
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include "rpc.h"

static void cache_set(SVCXPRT *xprt, unsigned long replylen);
static int cache_get(
    SVCXPRT *xprt,
    struct rpc_msg *msg,
    char **replyp,
    unsigned long *replylenp);


#define rpc_buffer(xprt) ((xprt)->xp_p1)
#ifndef MAX
#  define MAX(a, b)     (((a) > (b)) ? (a) : (b))
#endif

static bool_t		svcudp_recv(SVCXPRT *xprt, struct rpc_msg *msg);
static enum xprt_stat	svcudp_stat(SVCXPRT *xprt);
static bool_t		svcudp_getargs(
			    SVCXPRT *xprt,
			    xdrproc_t xdr_args,
			    char* args_ptr);
static bool_t		svcudp_reply(SVCXPRT *xprt, struct rpc_msg *msg);
static bool_t		svcudp_freeargs(
			    SVCXPRT *xprt,
			    xdrproc_t xdr_args,
			    char* args_ptr);
static void		svcudp_destroy(SVCXPRT *xprt);

static struct xp_ops svcudp_op = {
	svcudp_recv,
	svcudp_stat,
	svcudp_getargs,
	svcudp_reply,
	svcudp_freeargs,
	svcudp_destroy
};

extern int errno;

/*
 * kept in xprt->xp_p2
 */
struct svcudp_data {
	unsigned   su_iosz;	/* byte size of send.recv buffer */
	unsigned long	su_xid;		/* transaction id */
	XDR	su_xdrs;	/* XDR handle */
	char	su_verfbody[MAX_AUTH_BYTES];	/* verifier body */
	char * 	su_cache;	/* cached data, NULL if no cache */
};
#define	su_data(xprt)	((struct svcudp_data *)(xprt->xp_p2))

/*
 * Usage:
 *	xprt = svcudp_create(sock);
 *
 * If sock<0 then a socket is created, else sock is used.
 * If the socket, sock is not bound to a port then svcudp_create
 * binds it to an arbitrary port.  In any (successful) case,
 * xprt->xp_sock is the registered socket number and xprt->xp_port is the
 * associated port number.
 * Once *xprt is initialized, it is registered as a transporter;
 * see (svc.h, xprt_register).
 * The routines returns NULL if a problem occurred.
 */
SVCXPRT *
svcudp_bufcreate(
	register int sock,
	unsigned sendsz,
	unsigned recvsz)
{
	bool_t madesock = FALSE;
	register SVCXPRT *xprt;
	register struct svcudp_data *su;
	struct sockaddr_in addr;
	socklen_t len = sizeof(struct sockaddr_in);

	if (sock == RPC_ANYSOCK) {
		if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
			perror("svcudp_create: socket creation problem");
			return ((SVCXPRT *)NULL);
		}
		madesock = TRUE;
	}
	memset((char *)&addr, 0, sizeof (addr));
	addr.sin_family = AF_INET;
	if (bindresvport(sock, &addr)) {
		addr.sin_port = 0;
		(void)bind(sock, (struct sockaddr *)&addr, len);
	}
	if (getsockname(sock, (struct sockaddr *)&addr, &len) != 0) {
		perror("svcudp_create - cannot getsockname");
		if (madesock)
			(void)close(sock);
		return ((SVCXPRT *)NULL);
	}
	xprt = (SVCXPRT *)mem_alloc(sizeof(SVCXPRT));
	if (xprt == NULL) {
		(void)fprintf(stderr, "svcudp_create: out of memory\n");
		if (madesock)
			(void)close(sock);
		return (NULL);
	}
	su = (struct svcudp_data *)mem_alloc(sizeof(*su));
	if (su == NULL) {
		(void)fprintf(stderr, "svcudp_create: out of memory\n");
		mem_free(xprt, sizeof(*xprt));
		if (madesock)
			(void)close(sock);
		return (NULL);
	}
	su->su_iosz = ((MAX(sendsz, recvsz) + 3) / 4) * 4;
	if ((rpc_buffer(xprt) = mem_alloc(su->su_iosz)) == NULL) {
		(void)fprintf(stderr, "svcudp_create: out of memory\n");
		mem_free(su, sizeof(*su));
		mem_free(xprt, sizeof(*xprt));
		if (madesock)
			(void)close(sock);
		return (NULL);
	}
	xdrmem_create(
	    &(su->su_xdrs), rpc_buffer(xprt), su->su_iosz, XDR_DECODE);
	su->su_cache = NULL;
	xprt->xp_p2 = (char*)su;
	xprt->xp_verf.oa_base = su->su_verfbody;
	xprt->xp_ops = &svcudp_op;
	xprt->xp_port = ntohs(addr.sin_port);
	xprt->xp_sock = sock;
	xprt_register(xprt);
	return (xprt);
}

SVCXPRT *
svcudp_create(
	int sock)
{

	return(svcudp_bufcreate(sock, UDPMSGSIZE, UDPMSGSIZE));
}

/*ARGSUSED*/
static enum xprt_stat
svcudp_stat(
	SVCXPRT *xprt)
{
	return (XPRT_IDLE); 
}

static bool_t
svcudp_recv(
	register SVCXPRT *xprt,
	struct rpc_msg *msg)
{
	register struct svcudp_data *su = su_data(xprt);
	register XDR *xdrs = &(su->su_xdrs);
	register ssize_t rlen;
	char *reply;
	unsigned long replylen;
	socklen_t len;

    again:
	len = xprt->xp_addrlen = sizeof(struct sockaddr_in);
	rlen = recvfrom(xprt->xp_sock, rpc_buffer(xprt), (int) su->su_iosz,
	    0, (struct sockaddr *)&(xprt->xp_raddr), &len);
	if (rlen == -1 && errno == EINTR)
		goto again;
	if (rlen < 4*sizeof(uint32_t))
		return (FALSE);
	xprt->xp_addrlen = len;
	xdrs->x_op = XDR_DECODE;
	XDR_SETPOS(xdrs, 0);
	if (! xdr_callmsg(xdrs, msg))
		return (FALSE);
	su->su_xid = msg->rm_xid;
	if (su->su_cache != NULL) {
		if (cache_get(xprt, msg, &reply, &replylen)) {
			(void) sendto(xprt->xp_sock, reply, replylen, 0,
			  (struct sockaddr *) &xprt->xp_raddr, xprt->xp_addrlen);
			return (TRUE);
		}
	}
	return (TRUE);
}

static bool_t
svcudp_reply(
	register SVCXPRT *xprt, 
	struct rpc_msg *msg) 
{
	register struct svcudp_data *su = su_data(xprt);
	register XDR *xdrs = &(su->su_xdrs);
	register int slen;
	register bool_t stat = FALSE;

	xdrs->x_op = XDR_ENCODE;
	XDR_SETPOS(xdrs, 0);
	msg->rm_xid = su->su_xid;
	if (xdr_replymsg(xdrs, msg)) {
		slen = (int)XDR_GETPOS(xdrs);
		if (sendto(xprt->xp_sock, rpc_buffer(xprt), slen, 0,
		    (struct sockaddr *)&(xprt->xp_raddr), xprt->xp_addrlen)
		    == (ssize_t)slen) {
			stat = TRUE;
			if (su->su_cache) {
				cache_set(xprt, (unsigned long) slen);
			}
		}
	}
	return (stat);
}

static bool_t
svcudp_getargs(
	SVCXPRT *xprt,
	xdrproc_t xdr_args,
	char* args_ptr)
{

	return ((*xdr_args)(&(su_data(xprt)->su_xdrs), args_ptr));
}

static bool_t
svcudp_freeargs(
	SVCXPRT *xprt,
	xdrproc_t xdr_args,
	char* args_ptr)
{
	register XDR *xdrs = &(su_data(xprt)->su_xdrs);

	xdrs->x_op = XDR_FREE;
	return ((*xdr_args)(xdrs, args_ptr));
}

static void
svcudp_destroy(
	register SVCXPRT *xprt)
{
	register struct svcudp_data *su = su_data(xprt);

	xprt_unregister(xprt);
	(void)close(xprt->xp_sock);
	XDR_DESTROY(&(su->su_xdrs));
	mem_free(rpc_buffer(xprt), su->su_iosz);
	mem_free((char*)su, sizeof(struct svcudp_data));
	mem_free((char*)xprt, sizeof(SVCXPRT));
}


/***********this could be a separate file*********************/

/*
 * Fifo cache for udp server
 * Copies pointers to reply buffers into fifo cache
 * Buffers are sent again if retransmissions are detected.
 */

#define SPARSENESS 4	/* 75% sparse */

#define CACHE_PERROR(msg)	\
	(void) fprintf(stderr,"%s\n", msg)

#define ALLOC(type, size)	\
	(type *) mem_alloc((sizeof(type) * (size)))

#define BZERO(addr, type, size)	 \
	memset((char *) addr, 0, sizeof(type) * (size))

/*
 * An entry in the cache
 */
typedef struct cache_node *cache_ptr;
struct cache_node {
	/*
	 * Index into cache is xid, proc, vers, prog and address
	 */
	unsigned long cache_xid;
	unsigned long cache_proc;
	unsigned long cache_vers;
	unsigned long cache_prog;
	struct sockaddr_in cache_addr;
	/*
	 * The cached reply and length
	 */
	char * cache_reply;
	unsigned long cache_replylen;
	/*
 	 * Next node on the list, if there is a collision
	 */
	cache_ptr cache_next;	
};



/*
 * The entire cache
 */
struct udp_cache {
	unsigned long uc_size;		/* size of cache */
	cache_ptr *uc_entries;	/* hash table of entries in cache */
	cache_ptr *uc_fifo;	/* fifo list of entries in cache */
	unsigned long uc_nextvictim;	/* points to next victim in fifo list */
	unsigned long uc_prog;		/* saved program number */
	unsigned long uc_vers;		/* saved version number */
	unsigned long uc_proc;		/* saved procedure number */
	struct sockaddr_in uc_addr; /* saved caller's address */
};


/*
 * the hashing function
 */
#define CACHE_LOC(transp, xid)	\
 (xid % (SPARSENESS*((struct udp_cache *) su_data(transp)->su_cache)->uc_size))	


/*
 * Enable use of the cache. 
 * Note: there is no disable.
 */
int
svcudp_enablecache(
	SVCXPRT *transp,
	unsigned long size)
{
	struct svcudp_data *su = su_data(transp);
	struct udp_cache *uc;

	if (su->su_cache != NULL) {
		CACHE_PERROR("enablecache: cache already enabled");
		return(0);	
	}
	uc = ALLOC(struct udp_cache, 1);
	if (uc == NULL) {
		CACHE_PERROR("enablecache: could not allocate cache");
		return(0);
	}
	uc->uc_size = size;
	uc->uc_nextvictim = 0;
	uc->uc_entries = ALLOC(cache_ptr, size * SPARSENESS);
	if (uc->uc_entries == NULL) {
		CACHE_PERROR("enablecache: could not allocate cache data");
		mem_free(uc, sizeof(*uc));
		return(0);
	}
	BZERO(uc->uc_entries, cache_ptr, size * SPARSENESS);
	uc->uc_fifo = ALLOC(cache_ptr, size);
	if (uc->uc_fifo == NULL) {
		mem_free(uc->uc_entries, sizeof(*uc->entries)*size*SPARSENESS);
		mem_free(uc, sizeof(*uc));
		CACHE_PERROR("enablecache: could not allocate cache fifo");
		return(0);
	}
	BZERO(uc->uc_fifo, cache_ptr, size);
	su->su_cache = (char *) uc;
	return(1);
}


/*
 * Set an entry in the cache
 */
static void
cache_set(
	SVCXPRT *xprt,
	unsigned long replylen)	
{
	register cache_ptr victim;	
	register cache_ptr *vicp;
	register struct svcudp_data *su = su_data(xprt);
	struct udp_cache *uc = (struct udp_cache *) su->su_cache;
	unsigned long loc;
	char *newbuf;

	/*
 	 * Find space for the new entry, either by
	 * reusing an old entry, or by mallocing a new one
	 */
	victim = uc->uc_fifo[uc->uc_nextvictim];
	if (victim != NULL) {
		loc = CACHE_LOC(xprt, victim->cache_xid);
		for (vicp = &uc->uc_entries[loc]; 
		  *vicp != NULL && *vicp != victim; 
		  vicp = &(*vicp)->cache_next) 
				;
		if (*vicp == NULL) {
			CACHE_PERROR("cache_set: victim not found");
			return;
		}
		*vicp = victim->cache_next;	/* remote from cache */
		newbuf = victim->cache_reply;
	} else {
		victim = ALLOC(struct cache_node, 1);
		if (victim == NULL) {
			CACHE_PERROR("cache_set: victim alloc failed");
			return;
		}
		newbuf = mem_alloc(su->su_iosz);
		if (newbuf == NULL) {
			CACHE_PERROR("cache_set: could not allocate new rpc_buffer");
			mem_free(victim, sizeof(*victim));
			return;
		}
	}

	/*
	 * Store it away
	 */
	victim->cache_replylen = replylen;
	victim->cache_reply = rpc_buffer(xprt);
	rpc_buffer(xprt) = newbuf;
	xdrmem_create(&(su->su_xdrs), rpc_buffer(xprt), su->su_iosz, XDR_ENCODE);
	victim->cache_xid = su->su_xid;
	victim->cache_proc = uc->uc_proc;
	victim->cache_vers = uc->uc_vers;
	victim->cache_prog = uc->uc_prog;
	victim->cache_addr = uc->uc_addr;
	loc = CACHE_LOC(xprt, victim->cache_xid);
	victim->cache_next = uc->uc_entries[loc];	
	uc->uc_entries[loc] = victim;
	uc->uc_fifo[uc->uc_nextvictim++] = victim;
	uc->uc_nextvictim %= uc->uc_size;
}

/*
 * Try to get an entry from the cache
 * return 1 if found, 0 if not found
 */
static int
cache_get(
	SVCXPRT *xprt,
	struct rpc_msg *msg,
	char **replyp,
	unsigned long *replylenp)
{
	unsigned long loc;
	register cache_ptr ent;
	register struct svcudp_data *su = su_data(xprt);
	register struct udp_cache *uc = (struct udp_cache *) su->su_cache;

#	define EQADDR(a1, a2)	(memcmp((char*)&a1, (char*)&a2, sizeof(a1)) == 0)

	loc = CACHE_LOC(xprt, su->su_xid);
	for (ent = uc->uc_entries[loc]; ent != NULL; ent = ent->cache_next) {
		if (ent->cache_xid == su->su_xid &&
		  ent->cache_proc == uc->uc_proc &&
		  ent->cache_vers == uc->uc_vers &&
		  ent->cache_prog == uc->uc_prog &&
		  EQADDR(ent->cache_addr, uc->uc_addr)) {
			*replyp = ent->cache_reply;
			*replylenp = ent->cache_replylen;
			return(1);
		}
	}
	/*
	 * Failed to find entry
	 * Remember a few things so we can do a set later
	 */
	uc->uc_proc = msg->rm_call.cb_proc;
	uc->uc_vers = msg->rm_call.cb_vers;
	uc->uc_prog = msg->rm_call.cb_prog;
	uc->uc_addr = xprt->xp_raddr;
	return(0);
}

