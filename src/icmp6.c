/*
 *  Alarm Pinger (c) 2002 Jacek Konieczny <jajcus@jajcus.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 */

#include "config.h"

#ifdef HAVE_IPV6
#include "apinger.h"

#ifdef HAVE_STDLIB_H
# include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
# include <string.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_ICMP6_H
# include <netinet/icmp6.h>
#endif
#ifdef HAVE_NETINET_IP6_H
# include <netinet/ip6.h>
#endif
#ifdef HAVE_ERRNO_H
# include <errno.h>
#endif
#ifdef HAVE_SCHED_H
# include <sched.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif
#ifdef HAVE_SYS_UIO_H
# include <sys/uio.h>
#endif
#include "debug.h"

#ifdef HAVE_LINUX_FILTER_H
# ifdef HAVE_LINUX_TYPES_H
#  include <linux/types.h>
# endif
# include <linux/filter.h>
#endif /* HAVE_LINUX_FILTER_H */

void install_filter6(){

#ifdef HAVE_LINUX_FILTER_H
        static struct sock_filter insns[] = {
                BPF_STMT(BPF_LD|BPF_H|BPF_ABS, 4),  /* Load icmp echo ident */
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, 0xAAAA, 0, 1),  /* Ours? */
                BPF_STMT(BPF_RET|BPF_K, ~0U),  /* Yes, it passes. */
                BPF_STMT(BPF_LD|BPF_B|BPF_ABS, 0),  /* Load icmp type */
                BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, ICMP6_ECHO_REPLY, 1, 0), /* Echo? */
                BPF_STMT(BPF_RET|BPF_K, ~0U), /* No. It passes. This must not happen. */
                BPF_STMT(BPF_RET|BPF_K, 0), /* Echo with wrong ident. Reject. */
        };
        static struct sock_fprog filter = {
                sizeof insns / sizeof(insns[0]),
                insns
        };

        insns[1] = (struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K, htons(ident), 0, 1);

        if (setsockopt(icmp6_sock, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter)))
                myperror("WARNING: failed to install socket filter\n");
#endif /* HAVE_LINUX_FILTER_H */

}


void send_icmp6_probe(struct target *t,int seq){
static char buf[1024];
struct icmp6_hdr *p=(struct icmp6_hdr *)buf;
struct trace_info ti;
struct timeval cur_time;
int size;
int ret;

	p->icmp6_type=ICMP6_ECHO_REQUEST;
	p->icmp6_code=0;
	p->icmp6_cksum=0;
	p->icmp6_seq=seq%65536;
	p->icmp6_id=ident;

#ifdef HAVE_SCHED_YIELD
	/* Give away our time now, or we may be stopped between gettimeofday() and sendto() */ 
	sched_yield();
#endif
	gettimeofday(&cur_time,NULL);
	ti.timestamp=cur_time;
	ti.target_id=t;
	ti.seq=seq;
	memcpy(p+1,&ti,sizeof(ti));
	size=sizeof(*p)+sizeof(ti);

	ret=sendto(t->socket,p,size,MSG_DONTWAIT,
			(struct sockaddr *)&t->addr.addr6,sizeof(t->addr.addr6));
	if (ret<0){
		if (config->debug) myperror("sendto");
		switch (errno) {
                case EBADF:
                case ENOTSOCK:
                        if (t->socket)
                                close(t->socket);
                        make_icmp6_socket(t);
                        break;
                }
	}
}

void recv_icmp6(struct target *t, struct timeval *time_recv, int timedelta){
int len,icmplen,datalen;
char buf[1024];
char abuf[100];
const char *name;
struct sockaddr_in6 from;
struct icmp6_hdr *icmp;
socklen_t sl;
reloophack6:

	sl=sizeof(from);
	len=recvfrom(t->socket,buf,1024,0,(struct sockaddr *)&from,&sl);
	if (len<0){
		if (errno==EAGAIN) return;
		myperror("recvfrom");
		return;
	}
	if (len==0) return;
	icmplen=len;
	icmp=(struct icmp6_hdr *)buf;
	if (icmp->icmp6_type != ICMP6_ECHO_REPLY) return;
	if (icmp->icmp6_id != ident){
		debug("Alien echo-reply received from xxx. Expected %i, received %i", ident, icmp->icmp6_id);
		goto reloophack6;
		return;	
	}
#if 0
	name=inet_ntop(AF_INET6,&from.sin6_addr,abuf,100);
	debug("Ping reply from %s",name);
#endif
	datalen=icmplen-sizeof(*icmp);
	if (datalen!=sizeof(struct trace_info)){
		debug("Packet data truncated.");
		return;
	}
	analyze_reply(time_recv,icmp->icmp6_seq,(struct trace_info*)(icmp+1), timedelta);
}


int make_icmp6_socket(struct target *t){
int opt;

	t->socket = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (t->socket <0)
		logit("Could not create socket on address(%s) for monitoring address %s(%s) with error %m", t->config->srcip, t->name, t->description);
	else {
		opt=2;
#if defined(SOL_RAW) && defined(IPV6_CHECKSUM)
		if (setsockopt(t->socket, SOL_RAW, IPV6_CHECKSUM, &opt, sizeof(int)))
			myperror("setsockopt(IPV6_CHECKSUM)");
#endif
		if (bind(t->socket, (struct sockaddr *)&t->ifaddr.addr6, sizeof(t->ifaddr.addr6)) < 0)
			logit("Could not bind socket on address(%s) for monitoring address %s(%s) with error %m", t->config->srcip, t->name, t->description);
	}

	return t->socket;
}

#else /*HAVE_IPV6*/
#include "apinger.h"

int make_icmp6_socket(struct target *t){ return -1; }
void recv_icmp6(struct target *t){}
void send_icmp6_probe(struct target *t,int seq){}

#endif /*HAVE_IPV6*/
