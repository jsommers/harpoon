/*
 * $Id: netflowv5.h,v 1.1 2004-05-25 02:18:03 jsommers Exp $
 */

#ifndef __NETFLOWV5_H__
#define __NETFLOWV5_H__

// 24 bytes
struct NetflowV5Header
{
   short version;
   short flow_count;
   int sys_uptime;
   int unix_secs;
   int unix_nsecs;
   int flow_sequence;
   char engine_type;
   char engine_id;
   short sampling_interval;
};

// 48 bytes
struct NetflowV5Record
{
   unsigned int srcaddr;
   unsigned int dstaddr;
   unsigned int nexthop;
   short inputifid;
   short outputifid;
   unsigned int npkts;
   unsigned int noctets;
   unsigned int flowbegin;
   unsigned int flowend;
   unsigned short srcport;
   unsigned short dstport;
   char pad1;
   char tcp_flags;
   char proto;
   char tos;
   short src_as;
   short dst_as;
   char src_mask;
   char dst_mask;
   short pad2;
};


#endif //  __NETFLOWV5_H__
