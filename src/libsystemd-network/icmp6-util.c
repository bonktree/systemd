/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2014 Intel Corporation. All rights reserved.
***/

#include <errno.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_packet.h>

#include "fd-util.h"
#include "icmp6-util.h"
#include "in-addr-util.h"
#include "iovec-util.h"
#include "socket-util.h"

#define IN6ADDR_ALL_ROUTERS_MULTICAST_INIT \
        { { { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 } } }

#define IN6ADDR_ALL_NODES_MULTICAST_INIT \
        { { { 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } } }

static int icmp6_bind_router_message(const struct icmp6_filter *filter,
                                     const struct ipv6_mreq *mreq) {
        int ifindex = mreq->ipv6mr_interface;
        _cleanup_close_ int s = -EBADF;
        int r;

        assert(filter);
        assert(mreq);

        s = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_ICMPV6);
        if (s < 0)
                return -errno;

        if (setsockopt(s, IPPROTO_ICMPV6, ICMP6_FILTER, filter, sizeof(*filter)) < 0)
                return -errno;

        if (setsockopt(s, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, mreq, sizeof(*mreq)) < 0)
                return -errno;

        /* RFC 3315, section 6.7, bullet point 2 may indicate that an
           IPV6_PKTINFO socket option also applies for ICMPv6 multicast.
           Empirical experiments indicates otherwise and therefore an
           IPV6_MULTICAST_IF socket option is used here instead */
        r = setsockopt_int(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, ifindex);
        if (r < 0)
                return r;

        r = setsockopt_int(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, false);
        if (r < 0)
                return r;

        r = setsockopt_int(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, 255);
        if (r < 0)
                return r;

        r = setsockopt_int(s, IPPROTO_IPV6, IPV6_UNICAST_HOPS, 255);
        if (r < 0)
                return r;

        r = setsockopt_int(s, SOL_IPV6, IPV6_RECVHOPLIMIT, true);
        if (r < 0)
                return r;

        r = setsockopt_int(s, SOL_SOCKET, SO_TIMESTAMP, true);
        if (r < 0)
                return r;

        r = socket_bind_to_ifindex(s, ifindex);
        if (r < 0)
                return r;

        return TAKE_FD(s);
}

int icmp6_bind_router_solicitation(int ifindex) {
        struct icmp6_filter filter = {};
        struct ipv6_mreq mreq = {
                .ipv6mr_multiaddr = IN6ADDR_ALL_NODES_MULTICAST_INIT,
                .ipv6mr_interface = ifindex,
        };

        ICMP6_FILTER_SETBLOCKALL(&filter);
        ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filter);

        return icmp6_bind_router_message(&filter, &mreq);
}

int icmp6_bind_router_advertisement(int ifindex) {
        struct icmp6_filter filter = {};
        struct ipv6_mreq mreq = {
                .ipv6mr_multiaddr = IN6ADDR_ALL_ROUTERS_MULTICAST_INIT,
                .ipv6mr_interface = ifindex,
        };

        ICMP6_FILTER_SETBLOCKALL(&filter);
        ICMP6_FILTER_SETPASS(ND_ROUTER_SOLICIT, &filter);

        return icmp6_bind_router_message(&filter, &mreq);
}

int icmp6_send_router_solicitation(int s, const struct ether_addr *ether_addr) {
        struct sockaddr_in6 dst = {
                .sin6_family = AF_INET6,
                .sin6_addr = IN6ADDR_ALL_ROUTERS_MULTICAST_INIT,
        };
        struct {
                struct nd_router_solicit rs;
                struct nd_opt_hdr rs_opt;
                struct ether_addr rs_opt_mac;
        } _packed_ rs = {
                .rs.nd_rs_type = ND_ROUTER_SOLICIT,
                .rs_opt.nd_opt_type = ND_OPT_SOURCE_LINKADDR,
                .rs_opt.nd_opt_len = 1,
        };
        struct iovec iov = {
                .iov_base = &rs,
                .iov_len = sizeof(rs),
        };
        struct msghdr msg = {
                .msg_name = &dst,
                .msg_namelen = sizeof(dst),
                .msg_iov = &iov,
                .msg_iovlen = 1,
        };

        assert(s >= 0);
        assert(ether_addr);

        rs.rs_opt_mac = *ether_addr;

        if (sendmsg(s, &msg, 0) < 0)
                return -errno;

        return 0;
}

int icmp6_receive(
                int fd,
                void *buffer,
                size_t size,
                struct in6_addr *ret_sender,
                triple_timestamp *ret_timestamp) {

        /* This needs to be initialized with zero. See #20741. */
        CMSG_BUFFER_TYPE(CMSG_SPACE(sizeof(int)) + /* ttl */
                         CMSG_SPACE_TIMEVAL) control = {};
        struct iovec iov = {};
        union sockaddr_union sa = {};
        struct msghdr msg = {
                .msg_name = &sa.sa,
                .msg_namelen = sizeof(sa),
                .msg_iov = &iov,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
        };
        struct cmsghdr *cmsg;
        struct in6_addr addr = {};
        triple_timestamp t = {};
        ssize_t len;

        iov = IOVEC_MAKE(buffer, size);

        len = recvmsg_safe(fd, &msg, MSG_DONTWAIT);
        if (len < 0)
                return (int) len;

        if ((size_t) len != size)
                return -EINVAL;

        if (msg.msg_namelen == sizeof(struct sockaddr_in6) &&
            sa.in6.sin6_family == AF_INET6)  {

                addr = sa.in6.sin6_addr;
                if (!in6_addr_is_link_local(&addr) && !in6_addr_is_null(&addr))
                        return -EADDRNOTAVAIL;

        } else if (msg.msg_namelen > 0)
                return -EPFNOSUPPORT;

        /* namelen == 0 only happens when running the test-suite over a socketpair */

        assert(!(msg.msg_flags & MSG_TRUNC));

        CMSG_FOREACH(cmsg, &msg) {
                if (cmsg->cmsg_level == SOL_IPV6 &&
                    cmsg->cmsg_type == IPV6_HOPLIMIT &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
                        int hops = *CMSG_TYPED_DATA(cmsg, int);

                        if (hops != 255)
                                return -EMULTIHOP;
                }

                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_TIMESTAMP &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval))) {
                        struct timeval *tv = memcpy(&(struct timeval) {}, CMSG_DATA(cmsg), sizeof(struct timeval));
                        triple_timestamp_from_realtime(&t, timeval_load(tv));
                }
        }

        if (ret_timestamp) {
                if (triple_timestamp_is_set(&t))
                        *ret_timestamp = t;
                else
                        triple_timestamp_get(ret_timestamp);
        }

        if (ret_sender)
                *ret_sender = addr;
        return 0;
}
