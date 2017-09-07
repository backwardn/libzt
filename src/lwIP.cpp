/*
 * ZeroTier SDK - Network Virtualization Everywhere
 * Copyright (C) 2011-2017  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

// lwIP network stack driver 

#include <algorithm>

#include "libzt.h"
#include "VirtualTap.hpp"
#include "Utilities.hpp"
#include "lwIP.hpp"

#include "netif/ethernet.h"
#include "lwip/etharp.h"

#if defined(LIBZT_IPV6)
#include "lwip/ethip6.h"
#include "lwip/nd6.h"
#endif

void nd6_tmr(void);

err_t tapif_init(struct netif *netif)
{
  return ERR_OK;
}

err_t lwip_eth_tx(struct netif *netif, struct pbuf *p)
{
	struct pbuf *q;
	char buf[ZT_MAX_MTU+32];
	char *bufptr;
	int totalLength = 0;

	ZeroTier::VirtualTap *tap = (ZeroTier::VirtualTap*)netif->state;
	bufptr = buf;
	// Copy data from each pbuf, one at a time
	for(q = p; q != NULL; q = q->next) {
		memcpy(bufptr, q->payload, q->len);
		bufptr += q->len;
		totalLength += q->len;
	}
	// Split ethernet header and feed into handler
	struct eth_hdr *ethhdr;
	ethhdr = (struct eth_hdr *)buf;

	ZeroTier::MAC src_mac;
	ZeroTier::MAC dest_mac;
	src_mac.setTo(ethhdr->src.addr, 6);
	dest_mac.setTo(ethhdr->dest.addr, 6);

	tap->_handler(tap->_arg,NULL,tap->_nwid,src_mac,dest_mac,
		ZeroTier::Utils::ntoh((uint16_t)ethhdr->type),0,buf + sizeof(struct eth_hdr),totalLength - sizeof(struct eth_hdr));

	if(ZT_DEBUG_LEVEL >= ZT_MSG_TRANSFER) {
		char flagbuf[32];
		memset(&flagbuf, 0, 32);
		char macBuf[ZT_MAC_ADDRSTRLEN], nodeBuf[ZT_ID_LEN];
		mac2str(macBuf, ZT_MAC_ADDRSTRLEN, ethhdr->dest.addr);
		ZeroTier::MAC mac;
		mac.setTo(ethhdr->dest.addr, 6);
		mac.toAddress(tap->_nwid).toString(nodeBuf);
		DEBUG_TRANS("len=%5d dst=%s [%s TX <-- %s] proto=0x%04x %s %s", totalLength, macBuf, nodeBuf, tap->nodeId().c_str(), 
			ZeroTier::Utils::ntoh(ethhdr->type), beautify_eth_proto_nums(ZeroTier::Utils::ntoh(ethhdr->type)), flagbuf);
	}

	return ERR_OK;
}

namespace ZeroTier
{
	void lwIP::lwip_init_interface(VirtualTap *tap, const InetAddress &ip)
	{
		/* NOTE: It is a known issue that when assigned more than one IP address via 
		Central, this interface will be unable to transmit (including ARP). */
		Mutex::Lock _l(tap->_ips_m);

		if (std::find(tap->_ips.begin(),tap->_ips.end(),ip) == tap->_ips.end()) {
			tap->_ips.push_back(ip);
			std::sort(tap->_ips.begin(),tap->_ips.end());
			char ipbuf[INET6_ADDRSTRLEN], nmbuf[INET6_ADDRSTRLEN];
#if defined(LIBZT_IPV4)
			if (ip.isV4()) {
				static ip_addr_t ipaddr, netmask, gw;
				IP4_ADDR(&gw,127,0,0,1);
				ipaddr.addr = *((u32_t *)ip.rawIpData());
				netmask.addr = *((u32_t *)ip.netmask().rawIpData());
				netif_add(&(tap->lwipdev),&ipaddr, &netmask, &gw, NULL, tapif_init, ethernet_input);
				tap->lwipdev.state = tap;
				tap->lwipdev.output = etharp_output;
				tap->lwipdev.mtu = tap->_mtu;
				tap->lwipdev.name[0] = 'l';
				tap->lwipdev.name[1] = '4';
				tap->lwipdev.linkoutput = lwip_eth_tx;
				tap->lwipdev.hwaddr_len = 6;
				tap->_mac.copyTo(tap->lwipdev.hwaddr, tap->lwipdev.hwaddr_len);
				tap->lwipdev.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
				netif_set_default(&(tap->lwipdev));
				netif_set_up(&(tap->lwipdev));
				char macbuf[ZT_MAC_ADDRSTRLEN];
				mac2str(macbuf, ZT_MAC_ADDRSTRLEN, tap->lwipdev.hwaddr);
				DEBUG_INFO("mac=%s, addr=%s, nm=%s", macbuf, ip.toString(ipbuf), ip.netmask().toString(nmbuf));
			}
#endif
#if defined(LIBZT_IPV6)
			if(ip.isV6()) {
				static ip6_addr_t addr6;
				struct sockaddr_in6 in6;
				memcpy(in6.sin6_addr.s6_addr,ip.rawIpData(),16);
				in6_to_ip6((ip6_addr *)&addr6, &in6);
				tap->lwipdev6.mtu = tap->_mtu;
				tap->lwipdev6.name[0] = 'l';
				tap->lwipdev6.name[1] = '6';

				// hwaddr
				tap->lwipdev6.hwaddr_len = 6;
				tap->_mac.copyTo(tap->lwipdev6.hwaddr, tap->lwipdev6.hwaddr_len);

				// I/O
				tap->lwipdev6.linkoutput = lwip_eth_tx;
				tap->lwipdev6.output_ip6 = ethip6_output;
				netif_add(&(tap->lwipdev6), NULL, tapif_init, ethernet_input);

				//struct netif *netif, const ip6_addr_t *ip6addr, s8_t *chosen_idx
				//netif_add_ip6_address();
				
				// linklocal
				tap->lwipdev6.ip6_autoconfig_enabled = 1;
				netif_create_ip6_linklocal_address(&(tap->lwipdev6), 1);
				netif_ip6_addr_set_state(&(tap->lwipdev6), 0, IP6_ADDR_TENTATIVE); 

				// manually config addresses
				ip6_addr_copy(ip_2_ip6(tap->lwipdev6.ip6_addr[1]), addr6);
				netif_ip6_addr_set_state(&(tap->lwipdev6), 1, IP6_ADDR_TENTATIVE); 

				netif_set_default(&(tap->lwipdev6));
				netif_set_up(&(tap->lwipdev6));	
				
				// state and flags
				tap->lwipdev6.state = tap;
				tap->lwipdev6.flags = NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;

				char macbuf[ZT_MAC_ADDRSTRLEN];
				mac2str(macbuf, ZT_MAC_ADDRSTRLEN, tap->lwipdev6.hwaddr);
				DEBUG_INFO("mac=%s, addr=%s", macbuf, ip.toString(ipbuf));			
			}
#endif 
		}
	}

	int lwIP::lwip_add_dns_nameserver(struct sockaddr *addr)
	{
		return -1;
	}

	int lwIP::lwip_del_dns_nameserver(struct sockaddr *addr)
	{
		return -1;
	}

	void lwIP::lwip_loop(VirtualTap *tap)
	{
		// DEBUG_INFO();
		uint64_t prev_tcp_time = 0, prev_discovery_time = 0;
		while(tap->_run)
		{
			uint64_t now = OSUtils::now();
			uint64_t since_tcp = now - prev_tcp_time;
			uint64_t since_discovery = now - prev_discovery_time;
			uint64_t tcp_remaining = LWIP_TCP_TIMER_INTERVAL;
			uint64_t discovery_remaining = 5000;

#if defined(LIBZT_IPV6)
				#define DISCOVERY_INTERVAL 1000
#elif defined(LIBZT_IPV4)
				#define DISCOVERY_INTERVAL ARP_TMR_INTERVAL
#endif
			// Main TCP/ETHARP timer section
			if (since_tcp >= LWIP_TCP_TIMER_INTERVAL) {
				prev_tcp_time = now;
				tcp_tmr();
			} 
			else {
				tcp_remaining = LWIP_TCP_TIMER_INTERVAL - since_tcp;
			}
			if (since_discovery >= DISCOVERY_INTERVAL) {
				prev_discovery_time = now;
#if defined(LIBZT_IPV4)
					etharp_tmr();
#endif
#if defined(LIBZT_IPV6)
					nd6_tmr();
#endif
			} else {
				discovery_remaining = DISCOVERY_INTERVAL - since_discovery;
			}
			tap->_phy.poll((unsigned long)std::min(tcp_remaining,discovery_remaining));
			tap->Housekeeping();
		}
	}

	void lwIP::lwip_eth_rx(VirtualTap *tap, const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
	{
		struct pbuf *p,*q;
		if (!tap->_enabled){
			return;
		}
		struct eth_hdr ethhdr;
		from.copyTo(ethhdr.src.addr, 6);
		to.copyTo(ethhdr.dest.addr, 6);
		ethhdr.type = ZeroTier::Utils::hton((uint16_t)etherType);

		p = pbuf_alloc(PBUF_RAW, len+sizeof(struct eth_hdr), PBUF_POOL);
		if (p != NULL) {
			const char *dataptr = reinterpret_cast<const char *>(data);
			// First pbuf gets ethernet header at start
			q = p;
			if (q->len < sizeof(ethhdr)) {
				DEBUG_ERROR("dropped packet: first pbuf smaller than ethernet header");
				return;
			}
			memcpy(q->payload,&ethhdr,sizeof(ethhdr));
			memcpy((char*)q->payload + sizeof(ethhdr),dataptr,q->len - sizeof(ethhdr));
			dataptr += q->len - sizeof(ethhdr);
			// Remaining pbufs (if any) get rest of data
			while ((q = q->next)) {
				memcpy(q->payload,dataptr,q->len);
				dataptr += q->len;
			}
		} 
		if(ZT_DEBUG_LEVEL >= ZT_MSG_TRANSFER) {
			char flagbuf[32];
			memset(&flagbuf, 0, 32);
			char macBuf[ZT_MAC_ADDRSTRLEN], nodeBuf[ZT_ID_LEN];
			mac2str(macBuf, ZT_MAC_ADDRSTRLEN, ethhdr.dest.addr);
			ZeroTier::MAC mac;
			mac.setTo(ethhdr.src.addr, 6);
			mac.toAddress(tap->_nwid).toString(nodeBuf);
			DEBUG_TRANS("len=%5d dst=%s [%s RX --> %s] proto=0x%04x %s %s", len, macBuf, nodeBuf, tap->nodeId().c_str(), 
				ZeroTier::Utils::ntoh(ethhdr.type), beautify_eth_proto_nums(ZeroTier::Utils::ntoh(ethhdr.type)), flagbuf);
		}
		else {
			DEBUG_ERROR("dropped packet: no pbufs available");
			return;
		}
		{
#if defined(LIBZT_IPV4)
				if(tap->lwipdev.input(p, &(tap->lwipdev)) != ERR_OK) {
					DEBUG_ERROR("error while feeding frame into stack interface (ipv4)");
				}
#endif
#if defined(LIBZT_IPV6)
				if(tap->lwipdev6.input(p, &(tap->lwipdev6)) != ERR_OK) {
					DEBUG_ERROR("error while feeding frame into stack interface (ipv6)");
				}
#endif
		}
	}

	int lwIP::lwip_Socket(void **pcb, int socket_family, int socket_type, int protocol)
	{
		//DEBUG_INFO();
		if(!can_provision_new_socket()) {
			DEBUG_ERROR("unable to create new socket due to limitation of network stack");
			return -1;
		}
		if(socket_type == SOCK_STREAM) {
			struct tcp_pcb *new_tcp_PCB = tcp_new();
			*pcb = new_tcp_PCB;
			return ERR_OK;
		}
		if(socket_type == SOCK_DGRAM) {
			struct udp_pcb *new_udp_PCB = udp_new();
			*pcb = new_udp_PCB;
			return ERR_OK;
		}
		return -1;
	}

	int lwIP::lwip_Connect(VirtualSocket *vs, const struct sockaddr *addr, socklen_t addrlen)
	{
		//DEBUG_INFO();
		ip_addr_t ba;
		char addrstr[INET6_ADDRSTRLEN];
		int port = 0, err = 0;

#if defined(LIBZT_IPV4)
			struct sockaddr_in *in4 = (struct sockaddr_in *)addr;
			if(addr->sa_family == AF_INET) {
				inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN); 
				DEBUG_EXTRA("connecting to %s : %d", addrstr, lwip_ntohs(in4->sin_port));
			}
			ba = convert_ip(in4); 
			port = lwip_ntohs(in4->sin_port);
#endif
#if defined(LIBZT_IPV6)
			struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&addr;
			in6_to_ip6((ip6_addr *)&ba, in6);
			if(addr->sa_family == AF_INET6) {        
				inet_ntop(AF_INET6, &(in6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
				DEBUG_EXTRA("connecting to %s : %d", addrstr, lwip_ntohs(in6->sin6_port));
			}
#endif
		if(vs->socket_type == SOCK_DGRAM) {
			// Generates no network traffic
			if((err = udp_connect((struct udp_pcb*)vs->pcb,(ip_addr_t *)&ba,port)) < 0) {
				DEBUG_ERROR("error while connecting to with UDP");
			}
			udp_recv((struct udp_pcb*)vs->pcb, lwip_cb_udp_recved, vs);
			return ERR_OK;
		}

		if(vs->socket_type == SOCK_STREAM) {
			struct tcp_pcb *tpcb = (struct tcp_pcb*)vs->pcb;
			tcp_sent(tpcb, lwip_cb_sent);
			tcp_recv(tpcb, lwip_cb_tcp_recved);
			tcp_err(tpcb, lwip_cb_err);
			tcp_poll(tpcb, lwip_cb_poll, LWIP_APPLICATION_POLL_FREQ);
			tcp_arg(tpcb, vs);
				
			//DEBUG_EXTRA(" pcb->state=%x", vs->TCP_pcb->state);
			//if(vs->TCP_pcb->state != CLOSED) {
			//  DEBUG_INFO(" cannot connect using this PCB, PCB!=CLOSED");
			//  tap->sendReturnValue(tap->_phy.getDescriptor(rpcSock), -1, EAGAIN);
			//  return;
			//}
			if((err = tcp_connect(tpcb,&ba,port,lwip_cb_connected)) < 0)
			{
				if(err == ERR_ISCONN) {
					// Already in connected state
					errno = EISCONN;
					return -1;
				} if(err == ERR_USE) {
					// Already in use
					errno = EADDRINUSE;
					return -1;
				} if(err == ERR_VAL) {
					// Invalid ipaddress parameter
					errno = EINVAL;
					return -1;
				} if(err == ERR_RTE) {
					// No route to host
					errno = ENETUNREACH;
					return -1;
				} if(err == ERR_BUF) {
					// No more ports available
					errno = EAGAIN;
					return -1;
				}
				if(err == ERR_MEM) {
					// TODO: Doesn't describe the problem well, but closest match
					errno = EAGAIN;
					return -1;
				}
				// We should only return a value if failure happens immediately
				// Otherwise, we still need to wait for a callback from lwIP.
				// - This is because an ERR_OK from tcp_connect() only verifies
				//   that the SYN packet was enqueued onto the stack properly,
				//   that's it!
				// - Most instances of a retval for a connect() should happen
				//   in the nc_connect() and lwip_cb_err() callbacks!
				DEBUG_ERROR("unable to connect");
				errno = EAGAIN;
				return -1;
			}
		} 
		return err;
	}

	int lwIP::lwip_Bind(VirtualTap *tap, VirtualSocket *vs, const struct sockaddr *addr, socklen_t addrlen)
	{
		//DEBUG_EXTRA("vs=%p", vs);
		ip_addr_t ba;
		char addrstr[INET6_ADDRSTRLEN];
		memset(addrstr, 0, INET6_ADDRSTRLEN);
		int port = 0, err = 0;

#if defined(LIBZT_IPV4)
			struct sockaddr_in *in4 = (struct sockaddr_in *)addr;
			if(addr->sa_family == AF_INET) {
				inet_ntop(AF_INET, &(in4->sin_addr), addrstr, INET_ADDRSTRLEN); 
				DEBUG_EXTRA("binding to %s : %d", addrstr, lwip_ntohs(in4->sin_port));
			}
			ba = convert_ip(in4); 
			port = lwip_ntohs(in4->sin_port);
#endif
#if defined(LIBZT_IPV6)
			struct sockaddr_in6 *in6 = (struct sockaddr_in6*)addr;
			in6_to_ip6((ip6_addr *)&ba, in6);
			if(addr->sa_family == AF_INET6) {        
				inet_ntop(AF_INET6, &(in6->sin6_addr), addrstr, INET6_ADDRSTRLEN);
				DEBUG_EXTRA("binding to %s : %d", addrstr, lwip_ntohs(in6->sin6_port));
			}
#endif
		if(vs->socket_type == SOCK_DGRAM) {
			err = udp_bind((struct udp_pcb*)vs->pcb, (const ip_addr_t *)&ba, port);
			if(err == ERR_USE) {
				err = -1;
				errno = EADDRINUSE; // port in use
			}
			else {
				// set the recv callback
				udp_recv((struct udp_pcb*)vs->pcb, lwip_cb_udp_recved, vs);
				err = ERR_OK; 
				errno = ERR_OK; // success
			}
		}
		else if (vs->socket_type == SOCK_STREAM) {
			err = tcp_bind((struct tcp_pcb*)vs->pcb, (const ip_addr_t *)&ba, port);
			if(err != ERR_OK) {
				DEBUG_ERROR("err=%d", err);
				if(err == ERR_USE){
					err = -1; 
					errno = EADDRINUSE;
				}
				if(err == ERR_MEM){
					err = -1; 
					errno = ENOMEM;
				}
				if(err == ERR_BUF){
					err = -1; 
					errno = ENOMEM;
				}
			} 
			else {
				err = ERR_OK; 
				errno = ERR_OK; // success
			}
		}
		return err;
	}

	int lwIP::lwip_Listen(VirtualSocket *vs, int backlog)
	{
		DEBUG_INFO("vs=%p", vs);
		struct tcp_pcb* listeningPCB;
#ifdef TCP_LISTEN_BACKLOG
		listeningPCB = tcp_listen_with_backlog((struct tcp_pcb*)vs->pcb, backlog);
#else
		listeningPCB = tcp_listen((struct tcp_pcb*)vs->pcb);
#endif
		if(listeningPCB != NULL) {
			vs->pcb = listeningPCB;
			tcp_accept(listeningPCB, lwip_cb_accept); // set callback
			tcp_arg(listeningPCB, vs);
			//fcntl(tap->_phy.getDescriptor(vs->sock), F_SETFL, O_NONBLOCK);
		}
		return 0;
	}

	VirtualSocket* lwIP::lwip_Accept(VirtualSocket *vs)
	{
		if(!vs) {
			DEBUG_ERROR("invalid virtual socket");
			handle_general_failure();
			return NULL;
		}
		// Retreive first of queued VirtualSockets from parent VirtualSocket
		VirtualSocket *new_vs = NULL;
		if(vs->_AcceptedConnections.size()) {
			new_vs = vs->_AcceptedConnections.front();
			vs->_AcceptedConnections.pop();
		}
		return new_vs;
	}

	int lwIP::lwip_Read(VirtualSocket *vs, bool lwip_invoked)
	{
		DEBUG_EXTRA("vs=%p", vs);
		int err = 0;
		if(!vs) {
			DEBUG_ERROR("no virtual socket");
			return -1;
		}
		if(!lwip_invoked) {
			DEBUG_INFO("!lwip_invoked");
			vs->tap->_tcpconns_m.lock();
			vs->_rx_m.lock(); 
		}
		if(vs->RXbuf->count()) {
			int max = vs->socket_type == SOCK_STREAM ? ZT_STACK_TCP_SOCKET_RX_SZ : ZT_STACK_TCP_SOCKET_RX_SZ;
			int wr = std::min((ssize_t)max, (ssize_t)vs->RXbuf->count());
			int n = vs->tap->_phy.streamSend(vs->sock, vs->RXbuf->get_buf(), wr);
			char str[22];
			memcpy(str, vs->RXbuf->get_buf(), 22);
			vs->RXbuf->consume(n);
			
			if(vs->socket_type == SOCK_DGRAM)
			{
				// TODO
			}
			if(vs->socket_type == SOCK_STREAM) { // Only acknolwedge receipt of TCP packets
				tcp_recved((struct tcp_pcb*)vs->pcb, n);
				DEBUG_TRANS("TCP RX %d bytes", n);
			}
		}
		if(vs->RXbuf->count() == 0) {
			DEBUG_INFO("wrote everything");
			vs->tap->_phy.setNotifyWritable(vs->sock, false); // nothing else to send to the app
		}
		if(!lwip_invoked) {
			vs->tap->_tcpconns_m.unlock();
			vs->_rx_m.unlock();
		}
		return err;
	}

	int lwIP::lwip_Write(VirtualSocket *vs, void *data, ssize_t len)
	{
		DEBUG_EXTRA("vs=%p, len=%d", vs, len);
		int err = 0;
		if(!vs) {
			DEBUG_ERROR("no virtual socket");
			return -1;
		}
		if(vs->socket_type == SOCK_DGRAM) 
		{
			//DEBUG_ERROR("socket_type==SOCK_DGRAM");
			// TODO: Packet re-assembly hasn't yet been tested with lwIP so UDP packets are limited to MTU-sized chunks
			int udp_trans_len = std::min(len, (ssize_t)ZT_MAX_MTU);
			//DEBUG_EXTRA("allocating pbuf chain of size=%d for UDP packet", udp_trans_len);
			struct pbuf * pb = pbuf_alloc(PBUF_TRANSPORT, udp_trans_len, PBUF_POOL);
			if(!pb){
				DEBUG_ERROR("unable to allocate new pbuf of size=%d", vs->TXbuf->count());
				return -1;
			}
			memcpy(pb->payload, data, udp_trans_len);
			int err = udp_send((struct udp_pcb*)vs->pcb, pb);
			
			if(err == ERR_MEM) {
				DEBUG_ERROR("error sending packet. out of memory");
			} else if(err == ERR_RTE) {
				DEBUG_ERROR("could not find route to destinations address");
			} else if(err != ERR_OK) {
				DEBUG_ERROR("error sending packet - %d", err);
			} 
			pbuf_free(pb);
			if(err == ERR_OK) {
				return udp_trans_len;
			}
		}
		if(vs->socket_type == SOCK_STREAM) 
		{
			DEBUG_ERROR("socket_type==SOCK_STREAM");
			// How much we are currently allowed to write to the VirtualSocket
			ssize_t sndbuf = ((struct tcp_pcb*)vs->pcb)->snd_buf;
			int err, r;
			if(!sndbuf) {
				// PCB send buffer is full, turn off readability notifications for the
				// corresponding PhySocket until lwip_cb_sent() is called and confirms that there is
				// now space on the buffer
				DEBUG_ERROR("lwIP stack is full, sndbuf == 0");
				vs->tap->_phy.setNotifyReadable(vs->sock, false);
				return -1;
			}
			int buf_w = vs->TXbuf->write((const unsigned char*)data, len);
			if (buf_w != len) {
				// because we checked ZT_TCP_TX_BUF_SZ above, this should not happen
				DEBUG_ERROR("TX wrote only %d but expected to write %d", buf_w, len);
				exit(0);
			}
			if(vs->TXbuf->count() <= 0) {
				return -1; // nothing to write
			}
			if(vs->sock) {
				r = std::min((ssize_t)vs->TXbuf->count(), sndbuf);
				// Writes data pulled from the client's socket buffer to LWIP. This merely sends the
				// data to LWIP to be enqueued and eventually sent to the network.
				if(r > 0) {
					err = tcp_write((struct tcp_pcb*)vs->pcb, vs->TXbuf->get_buf(), r, TCP_WRITE_FLAG_COPY);
					tcp_output((struct tcp_pcb*)vs->pcb);
					if(err != ERR_OK) {
						DEBUG_ERROR("error while writing to lwIP tcp_pcb, err=%d", err);
						if(err == -1)
							DEBUG_ERROR("lwIP out of memory");
						return -1;
					} else {
						vs->TXbuf->consume(r); // success
						return ERR_OK;
					}
				}
			}
		}
		return err;
	}

	int lwIP::lwip_Close(VirtualSocket *vs)
	{
		//DEBUG_INFO();
		if(vs->socket_type == SOCK_DGRAM) {
			udp_remove((struct udp_pcb*)vs->pcb);
		}
		// FIXME: check if already closed? vs->TCP_pcb->state != CLOSED
		if(vs->pcb) {
			//DEBUG_EXTRA("vs=%p, sock=%p, PCB->state = %d", 
			//  conn, sock, vs->TCP_pcb->state);
			if(((struct tcp_pcb*)vs->pcb)->state == SYN_SENT /*|| vs->TCP_pcb->state == CLOSE_WAIT*/) {
				DEBUG_EXTRA("ignoring close request. invalid PCB state for this operation. sock=%p", vs->sock);
				return -1;
			}
			struct tcp_pcb* tpcb = (struct tcp_pcb*)vs->pcb;
			if(tcp_close(tpcb) == ERR_OK) {
				// Unregister callbacks for this PCB
				tcp_arg(tpcb, NULL);
				tcp_recv(tpcb, NULL);
				tcp_err(tpcb, NULL);
				tcp_sent(tpcb, NULL);
				tcp_poll(tpcb, NULL, 1);
			}
			else {
				DEBUG_EXTRA("error while calling tcp_close() sock=%p", vs->sock);
			}
		}
		return 0;
	}

	/****************************************************************************/
	/* Callbacks from lwIP stack                                                */
	/****************************************************************************/

	err_t lwIP::lwip_cb_tcp_recved(void *arg, struct tcp_pcb *PCB, struct pbuf *p, err_t err)
	{
		DEBUG_INFO();
		VirtualSocket *vs = (VirtualSocket *)arg;
		int tot = 0;

		if(!vs) {
			DEBUG_ERROR("no virtual socket");
			return ERR_OK; // FIXME: Determine if this is correct behaviour expected by the stack 
		}

		struct pbuf* q = p;
		if(p == NULL) {
			/*
			if(((struct tcp_pcb*)vs->pcb)->state == CLOSE_WAIT) {
				// FIXME: Implement?
			}
			*/
			DEBUG_INFO("p == NULL");
			return ERR_ABRT;
		}

		vs->tap->_tcpconns_m.lock();
		vs->_rx_m.lock();

		// cycle through pbufs and write them to the RX buffer
		while(p != NULL) {
			if(p->len <= 0)
				break;
			int avail = ZT_TCP_RX_BUF_SZ - vs->RXbuf->count();
			int len = p->len;
			if(avail < len) {
				DEBUG_ERROR("not enough room (%d bytes) on RX buffer", avail);
			}
			// get it on the buffer, fast!
			memcpy(vs->RXbuf->get_buf(), p->payload, len);
			vs->RXbuf->produce(len);
			p = p->next;
			tot += len;
		}
		DEBUG_INFO("tot=%d", tot);

		if(tot) {
			int w, write_attempt_sz = vs->RXbuf->count() < ZT_MAX_MTU ? vs->RXbuf->count() : ZT_MAX_MTU;
			if((w = write(vs->sdk_fd, vs->RXbuf->get_buf(), write_attempt_sz)) < 0) {
				perror("write");
				DEBUG_ERROR("write(fd=%d)=%d, errno=%d", vs->sdk_fd, w, errno);
			}
			if(w > 0) {
				DEBUG_INFO("write_attempt_sz=%d, w=%d", write_attempt_sz, w);
				vs->RXbuf->consume(w);
			}
			
			//vs->tap->_phy.setNotifyWritable(vs->sock, true);
			//vs->tap->phyOnUnixWritable(vs->sock, NULL, true); // to app
		}
		
		vs->tap->_tcpconns_m.unlock();
		vs->_rx_m.unlock();

		pbuf_free(q);
		return ERR_OK;
	}

	err_t lwIP::lwip_cb_accept(void *arg, struct tcp_pcb *newPCB, err_t err)
	{
		VirtualSocket *vs = (VirtualSocket*)arg;
		struct sockaddr_storage ss;
#if defined(LIBZT_IPV4)
		struct sockaddr_in *in4 = (struct sockaddr_in *)&ss;
		in4->sin_addr.s_addr = newPCB->remote_ip.addr;
		in4->sin_port = newPCB->remote_port;
#endif
#if defined(LIBZT_IPV6)
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
		// TODO: check this
		memcpy(&(in6->sin6_addr.s6_addr), &(newPCB->remote_ip), sizeof(int32_t)*4);
		in6->sin6_port = newPCB->remote_port;
#endif		
		VirtualSocket *new_vs = new VirtualSocket();
		new_vs->socket_type = SOCK_STREAM;
		new_vs->pcb = newPCB;
		new_vs->tap = vs->tap;
		new_vs->sock = vs->tap->_phy.wrapSocket(new_vs->sdk_fd, new_vs);
		memcpy(&(new_vs->peer_addr), &ss, sizeof(new_vs->peer_addr));
		// add new VirtualSocket object to parent VirtualSocket so that we can find it via lwip_Accept()
		vs->_AcceptedConnections.push(new_vs);
		// set callbacks
		tcp_arg(newPCB, new_vs);
		tcp_recv(newPCB, lwip_cb_tcp_recved);
		tcp_err(newPCB, lwip_cb_err);
		tcp_sent(newPCB, lwip_cb_sent);
		tcp_poll(newPCB, lwip_cb_poll, 1);
		// let lwIP know that it can queue additional incoming PCBs
		tcp_accepted((struct tcp_pcb*)vs->pcb); 
		return 0;
	}
		
	// copy processed datagram to app socket
	void lwIP::lwip_cb_udp_recved(void * arg, struct udp_pcb * upcb, struct pbuf * p, const ip_addr_t * addr, u16_t port)
	{
		DEBUG_EXTRA("arg(vs)=%p, pcb=%p, port=%d)", arg, upcb, port);
		VirtualSocket *vs = (VirtualSocket *)arg;
		if(!vs) {
			DEBUG_ERROR("invalid virtual socket");
			return;
		}
		if(!p) {
			DEBUG_ERROR("!p");
			return;
		}
		struct pbuf* q = p;
		struct sockaddr_storage ss;

	#if defined(LIBZT_IPV4)
		struct sockaddr_in *in4 = (struct sockaddr_in *)&ss;
		in4->sin_addr.s_addr = addr->addr;
		in4->sin_port = port;
	#endif
	#if defined(LIBZT_IPV6)
		struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
		memcpy(&(in6->sin6_addr.s6_addr), &(addr->addr), sizeof(int32_t)*4);
		in6->sin6_port = port;
	#endif

		char udp_payload_buf[ZT_SOCKET_MSG_BUF_SZ];
		char *msg_ptr = udp_payload_buf;

		int tot_len = 0;
		while(p != NULL)
		{
			if(p->len <= 0) {
				break;
			}
			memcpy(msg_ptr, p->payload, p->len);
			msg_ptr += p->len;
			tot_len += p->len;
			p = p->next;
		}
		if(tot_len) {
			int w = 0;
			//DEBUG_INFO("tot_len=%d", tot_len);
			char udp_msg_buf[ZT_SOCKET_MSG_BUF_SZ]; // [sz : addr : payload]
			int32_t len = sizeof(struct sockaddr_storage) + tot_len;
			int32_t msg_tot_len = sizeof(int32_t) + len;
			memcpy(udp_msg_buf, &len, sizeof(int32_t)); // len: sockaddr+payload
			memcpy(udp_msg_buf + sizeof(int32_t), &ss, sizeof(struct sockaddr_storage)); // sockaddr
			memcpy(udp_msg_buf + sizeof(int32_t) + sizeof(struct sockaddr_storage), &udp_payload_buf, tot_len); // payload
			if((w = write(vs->sdk_fd, udp_msg_buf, msg_tot_len)) < 0) {
				perror("write");
				DEBUG_ERROR("write(fd=%d)=%d, errno=%d", vs->sdk_fd, w, errno);
			}
			//vs->tap->phyOnUnixWritable(vs->sock, NULL, true);
			//vs->tap->_phy.setNotifyWritable(vs->sock, true);
		}
		pbuf_free(q);
	}

	err_t lwIP::lwip_cb_sent(void* arg, struct tcp_pcb *PCB, u16_t len)
	{
		DEBUG_EXTRA("pcb=%p", PCB);
		VirtualSocket *vs = (VirtualSocket *)arg;
		Mutex::Lock _l(vs->tap->_tcpconns_m);
		if(vs && len) {
			int softmax = vs->socket_type == SOCK_STREAM ? ZT_TCP_TX_BUF_SZ : ZT_UDP_TX_BUF_SZ;
			if(vs->TXbuf->count() < softmax) {
				vs->tap->_phy.setNotifyReadable(vs->sock, true);
				vs->tap->_phy.whack();
			}
		}
		return ERR_OK;
	}

	err_t lwIP::lwip_cb_connected(void *arg, struct tcp_pcb *PCB, err_t err)
	{
		DEBUG_ATTN("pcb=%p", PCB);
		VirtualSocket *vs = (VirtualSocket *)arg;
		if(!vs) {
			DEBUG_ERROR("invalid virtual socket");
			return -1;
		}
		// add to unhandled connection set for zts_connect to pick up on
		vs->tap->_tcpconns_m.lock();
		vs->state = ZT_SOCK_STATE_UNHANDLED_CONNECTED;
		vs->tap->_VirtualSockets.push_back(vs);
		vs->tap->_tcpconns_m.unlock();
		return ERR_OK;
	}

	err_t lwIP::lwip_cb_poll(void* arg, struct tcp_pcb *PCB)
	{
		return ERR_OK;
	}

	void lwIP::lwip_cb_err(void *arg, err_t err)
	{
		DEBUG_ERROR("err=%d", err);
		VirtualSocket *vs = (VirtualSocket *)arg;
		if(!vs){
			DEBUG_ERROR("vs==NULL");
			errno = -1; // FIXME: Find more appropriate value
		}
		Mutex::Lock _l(vs->tap->_tcpconns_m);
		int fd = vs->tap->_phy.getDescriptor(vs->sock);
		DEBUG_ERROR("vs=%p, pcb=%p, fd=%d, err=%d", vs, vs->pcb, fd, err);
		DEBUG_ERROR("closing virtual socket");
		vs->tap->Close(vs);
		switch(err)
		{
			case ERR_MEM:
				DEBUG_ERROR("ERR_MEM->ENOMEM");
				errno = ENOMEM;
				break;
			case ERR_BUF:
				DEBUG_ERROR("ERR_BUF->ENOBUFS");
				errno = ENOBUFS;
				break;
			case ERR_TIMEOUT:
				DEBUG_ERROR("ERR_TIMEOUT->ETIMEDOUT");
				errno = ETIMEDOUT;
				break;
			case ERR_RTE:
				DEBUG_ERROR("ERR_RTE->ENETUNREACH");
				errno = ENETUNREACH;
				break;
			case ERR_INPROGRESS:
				DEBUG_ERROR("ERR_INPROGRESS->EINPROGRESS");
				errno = EINPROGRESS;
				break;
			case ERR_VAL:
				DEBUG_ERROR("ERR_VAL->EINVAL");
				errno = EINVAL;
				break;
			case ERR_WOULDBLOCK:
				DEBUG_ERROR("ERR_WOULDBLOCK->EWOULDBLOCK");
				errno = EWOULDBLOCK;
				break;
			case ERR_USE:
				DEBUG_ERROR("ERR_USE->EADDRINUSE");
				errno = EADDRINUSE;
				break;
			case ERR_ISCONN:
				DEBUG_ERROR("ERR_ISvs->EISCONN");
				errno = EISCONN;
				break;
			case ERR_ABRT:
				DEBUG_ERROR("ERR_ABRT->ECONNREFUSED");
				errno = ECONNREFUSED;
				break;

				// TODO: Below are errors which don't have a standard errno correlate

			case ERR_RST:
				// -1
				break;
			case ERR_CLSD:
				// -1
				break;
			case ERR_CONN:
				// -1
				break;
			case ERR_ARG:
				// -1
				break;
			case ERR_IF:
				// -1
				break;
			default:
				break;
		}
	}
}
