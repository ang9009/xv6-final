#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

static struct spinlock netlock;

struct packet_node {
  char* payload;
  uint16 payload_len;
  uint32 src_ip;
  uint16 src_port;
  struct packet_node* next;
};

// Data placed here must be in little-endian
struct port_data {
  struct packet_node* packet_queue;
  int queue_len;
  int bound;  // Whether bind() has been called on this port
};

// Data placed here must be in little-endian
static struct port_data ports[65536];  // Indexed by port number

void netinit(void) {
  initlock(&netlock, "netlock");
}

//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64 sys_bind(void) {
  //
  // Your code here.
  //
  int port;
  argint(0, &port);
  if (ports[port].bound) {
    panic("sys_bind(): tried to bind port that has already been bound");
  }
  ports[port].bound = 1;

  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64 sys_unbind(void) {
  //
  // Optional: Your code here.
  //
  // ! Make sure yoy update portdata -> bound here

  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//
uint64 sys_recv(void) {
  int dport, maxlen;
  int* src;
  short* sport;
  char* buf;

  argint(0, &dport);
  argaddr(1, (uint64*)&src);
  argaddr(2, (uint64*)&sport);
  argaddr(3, (uint64*)&buf);
  argint(4, &maxlen);

  acquire(&netlock);

  struct port_data* data = &ports[dport];
  if (!data->bound) {  // ! Free?
    release(&netlock);
    return -1;
  }

  while (!data->packet_queue) {
    sleep((void*)&data->packet_queue, &netlock);
  }

  // Remove packet from head of packet queue
  const struct packet_node* node = data->packet_queue;
  data->packet_queue = data->packet_queue->next;
  data->queue_len--;
  if (data->queue_len < 0) {
    panic("sys_recv(): packet queue length is negative");
  }
  const struct proc* p = myproc();
  pagetable_t pt = p->pagetable;

  copyout(pt, (uint64)src, (char*)&node->src_ip, sizeof(uint32));
  copyout(pt, (uint64)sport, (char*)&node->src_port, sizeof(uint16));
  uint64 bytes_copied = node->payload_len > maxlen ? maxlen : node->payload_len;
  // printf("sys_recv(): from port %d, payload_len: %d, maxlen: %d\n", dport,
  //        node->payload_len, maxlen);
  copyout(pt, (uint64)buf, (char*)node->payload, bytes_copied);

  kfree((void*)node->payload);
  kfree((void*)node);
  release(&netlock);

  return bytes_copied;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short in_cksum(const unsigned char* addr, int len) {
  int nleft = len;
  const unsigned short* w = (const unsigned short*)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1) {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char*)(&answer) = *(const unsigned char*)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64 sys_send(void) {
  struct proc* p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if (total > PGSIZE)
    return -1;

  char* buf = kalloc();
  if (buf == 0) {
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth* eth = (struct eth*)buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip* ip = (struct ip*)(eth + 1);
  ip->ip_vhl = 0x45;  // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char*)ip, sizeof(*ip));

  struct udp* udp = (struct udp*)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char* payload = (char*)(udp + 1);
  if (copyin(p->pagetable, payload, bufaddr, len) < 0) {
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void ip_rx(char* buf, int len) {
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if (seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  const struct ip ip_header = *((struct ip*)(buf + sizeof(struct eth)));
  if (ip_header.ip_p !=
      IPPROTO_UDP) {  // IP protocol is 1 byte long, no translation needed
    goto err;
  }

  const struct udp udp_header =
      *((struct udp*)(buf + sizeof(struct eth) + sizeof(struct ip)));
  uint16 dport = ntohs(udp_header.dport);

  acquire(&netlock);
  struct port_data* port_info = &ports[dport];
  // Not already processed by bind(), or queue full
  if (!port_info->bound || (port_info->queue_len == MAX_PACKET_Q_LEN)) {
    release(&netlock);
    goto err;
  }

  const int payload_len = ntohs(udp_header.ulen) - sizeof(struct udp);
  char* payload_src =
      (buf + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp));
  char* payload_dst = kalloc();
  memmove((void*)payload_dst, (void*)payload_src, payload_len);

  struct packet_node* packet = kalloc();
  if (!packet) {
    panic("ip_rx(): kalloc failed");
  }
  *packet = (struct packet_node){
      .next = (void*)0,  // Packet is new tail
      .payload = payload_dst,
      .payload_len = payload_len,
      .src_ip = ntohl(ip_header.ip_src),
      .src_port = ntohs(udp_header.sport),
  };

  // Add packet to queue
  if (!port_info->packet_queue) {
    port_info->packet_queue = packet;
  } else {
    struct packet_node* tail = port_info->packet_queue;
    while (tail->next) {
      tail = tail->next;
    }
    tail->next = packet;
  }

  port_info->queue_len++;

  release(&netlock);
  wakeup((void*)&port_info->packet_queue);

err:
  kfree((void*)buf);
  return;
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void arp_rx(char* inbuf) {
  static int seen_arp = 0;

  if (seen_arp) {
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth* ineth = (struct eth*)inbuf;
  struct arp* inarp = (struct arp*)(ineth + 1);

  char* buf = kalloc();
  if (buf == 0)
    panic("send_arp_reply");

  struct eth* eth = (struct eth*)buf;
  memmove(eth->dhost, ineth->shost,
          ETHADDR_LEN);  // ethernet destination = query source
  memmove(eth->shost, local_mac,
          ETHADDR_LEN);  // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp* arp = (struct arp*)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void net_rx(char* buf, int len) {
  struct eth* eth = (struct eth*)buf;

  if (len >= sizeof(struct eth) + sizeof(struct arp) &&
      ntohs(eth->type) == ETHTYPE_ARP) {
    arp_rx(buf);
  } else if (len >= sizeof(struct eth) + sizeof(struct ip) &&
             ntohs(eth->type) == ETHTYPE_IP) {
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
