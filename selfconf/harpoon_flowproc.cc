/*
 * $Id: harpoon_flowproc.cc,v 1.10 2004-08-26 16:24:44 jsommers Exp $
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <string>
#include <set>
#include <list>
#include <algorithm>
#include <math.h>

#include "config.h"

#if HAVE_FLOAT_H
#include <float.h>
#ifndef __DBL_MAX__
#define __DBL_MAX__ DBL_MAX
#endif
#endif

#include "netflowv5.h"


#if HAVE_FLOWTOOLS
extern "C"
{
  #include <ftconfig.h>
  #include <ftlib.h>
}
#endif

// default idle time to demarcate sessions
double USER_IDLE_MAX = 60.0;


#define __FAVOR_BSD
#include <netinet/tcp.h>


struct FlowBlock
{
   FlowBlock(const NetflowV5Record &nfr, double &begin, double &end)
   {
      srcaddr = nfr.srcaddr; 
      dstaddr = nfr.dstaddr; 
      srcport = nfr.srcport; 
      dstport = nfr.dstport; 
      flowbegin = begin;
      flowend = end;
      npkts = nfr.npkts;
      noctets = nfr.noctets;
      proto = nfr.proto;
      tcp_flags = nfr.tcp_flags;
   }
#if HAVE_FLOWTOOLS
   FlowBlock(struct fts3rec_all &frec, double &begin, double &end)
   {
      srcaddr = *frec.srcaddr; 
      dstaddr = *frec.dstaddr; 
      srcport = *frec.srcport; 
      dstport = *frec.dstport; 
      flowbegin = begin;
      flowend = end;
      npkts = *frec.dPkts;
      noctets = *frec.dOctets;
      proto = *frec.prot;
      tcp_flags = *frec.tcp_flags;
   }
#endif

   bool operator<(FlowBlock &fb2)
   {
      return (flowbegin < fb2.flowbegin);
   }

   unsigned int srcaddr;
   unsigned int dstaddr;
   unsigned short srcport;
   unsigned short dstport;
   double flowbegin;
   double flowend;
   unsigned int npkts;
   unsigned int noctets;
   unsigned char proto;
   unsigned char tcp_flags;
};


struct FlowDescript
{
    FlowDescript() : m_pkts(0), m_octets(0), m_start(0), m_duration(0), m_tcpflags(0), m_srcport(0), m_dstport(0), m_proto(0) {}
    FlowDescript(unsigned int pkts, unsigned int octets, double start, double duration, unsigned char tcpflags, unsigned short sport, unsigned short dport, unsigned char proto) : m_pkts(pkts), m_octets(octets), m_start(start), m_duration(duration), m_tcpflags(tcpflags), m_srcport(sport), m_dstport(dport), m_proto(proto) {}
    void reset() 
        {
            m_pkts = 0; m_octets = 0; m_start = 0; m_duration = 0; m_tcpflags = 0; 
        }
    void accumulate(FlowDescript &fd)
        {
            m_pkts += fd.m_pkts;
            m_octets += fd.m_octets;
            m_tcpflags |= fd.m_tcpflags;
            double end = std::max(m_start+m_duration,fd.m_start+fd.m_duration);
            m_start = std::min(m_start, fd.m_start);
            m_duration = end - m_start;
        }
    bool isEmpty() const
        {
            return (m_pkts == 0 && m_octets == 0 &&
                    m_tcpflags == 0 && m_start == 0 && m_duration == 0);
        }

    bool operator<(const FlowDescript &fd)
        {
            return (m_start < fd.m_start);
        }
 
    unsigned int m_pkts;
    unsigned int m_octets;
    double m_start;
    double m_duration;
    unsigned char m_tcpflags;
    unsigned short m_srcport;
    unsigned short m_dstport;
    unsigned char m_proto;
};


struct PortPair
{
    PortPair(unsigned short srcport, unsigned short dstport, unsigned char proto)
        {
            m_srcport = srcport;
            m_dstport = dstport;
            m_proto = proto;
            m_flows = 0;
        }

    PortPair()
        {
            m_srcport = m_dstport = m_proto = 0;
            m_flows = 0;
        }

    void reset()
        {
            m_srcport = m_dstport = m_proto = 0;
        }

    bool operator==(const PortPair &pp2) const
        {
            return (m_srcport == pp2.m_srcport &&            
                    m_dstport == pp2.m_dstport &&
                    m_proto == pp2.m_proto);
        }

    bool operator<(const PortPair &pp2) const
        {
            unsigned char a[5], b[5];
            memcpy(&a[0], &m_srcport, 2);
            memcpy(&a[2], &m_dstport, 2);
            a[4] = m_proto;

            memcpy(&b[0], &pp2.m_srcport, 2);
            memcpy(&b[2], &pp2.m_dstport, 2);
            b[4] = pp2.m_proto;

            return (memcmp(&a[0], &b[0], 5) < 0);
        }

    unsigned short m_srcport;
    unsigned short m_dstport;
    unsigned char m_proto;
    unsigned char m_fill1;
    unsigned char m_fill2;
    unsigned char m_fill3;

    std::list<FlowDescript> *m_flows;
};


struct HostPair
{
    HostPair(unsigned int ipsrc, unsigned int ipdst)
        {
            m_src = ipsrc;
            m_dst = ipdst;
            m_portpairs = 0;
        }

    HostPair()
        {
            m_src = m_dst = 0;
            m_portpairs = 0;
        }

    void reset()
        {
            m_src = m_dst = 0;
        }

    bool operator==(const HostPair &hp2) const
        {
            return (hp2.m_src == m_src && 
                    hp2.m_dst == m_dst);
        }

    bool operator<(const HostPair &hp2) const
        {
            unsigned char a[8], b[8];
            memcpy(&a[0], &m_src, 4); 
            memcpy(&a[4], &m_dst, 4); 
            memcpy(&b[0], &hp2.m_src, 4); 
            memcpy(&b[4], &hp2.m_dst, 4); 

            return (memcmp(&a[0], &b[0], 8) < 0);
        }

    
    unsigned int m_src;
    unsigned int m_dst;
    std::set<PortPair> *m_portpairs;
};


std::ostream& operator<<(std::ostream &os, const HostPair &hp)
{
    struct in_addr ina1;
    struct in_addr ina2;
    ina1.s_addr = hp.m_src;
    ina2.s_addr = hp.m_dst;
    os << inet_ntoa(ina1) << ' ';
    os << inet_ntoa(ina2);
    return (os);
}


std::ostream& operator<<(std::ostream &os, const PortPair &pp)
{
    os << pp.m_srcport << ' ' << pp.m_dstport << ' ' << int(pp.m_proto);
    return os;
}


std::ostream& operator<<(std::ostream &os, const FlowDescript &fo)
{
    os << fo.m_srcport << ' ' << fo.m_dstport << ' ' << int(fo.m_proto) << ' '
       << std::setprecision(15) << fo.m_start << ' ' 
       << std::setprecision(6) << fo.m_duration << ' '
       << fo.m_pkts << ' ' << fo.m_octets << ' ';
    if (fo.m_tcpflags & TH_SYN)
        os << 'S';
    if (fo.m_tcpflags & TH_FIN)
        os << 'F';
    if (fo.m_tcpflags & TH_ACK)
        os << 'A';
    if (fo.m_tcpflags & TH_RST)
        os << 'R';
    
    return os;
}

typedef std::list<FlowDescript>::iterator FDITER;
typedef std::set<HostPair>::iterator HPITER;
typedef std::set<PortPair>::iterator PPITER;


#if HAVE_FLOWTOOLS
int ftools_decode(void *voidp, std::list<FlowBlock> &flows, 
                  int &tottcpflows, double &min_begin)
{
    struct ftio *ftinp = (struct ftio*)voidp;

    struct fttime ftt_begin, ftt_end;
    fts3rec_all cur;
    struct fts3rec_offsets fo;
    struct ftver ftv;
    char *rec;
    int nflows = 0;

    if (ftio_check_xfield(ftinp, FT_XFIELD_DPKTS |
        FT_XFIELD_DOCTETS | FT_XFIELD_FIRST | FT_XFIELD_LAST | 
        FT_XFIELD_SRCADDR | FT_XFIELD_DSTADDR |
        FT_XFIELD_SRCPORT | FT_XFIELD_DSTPORT | 
        FT_XFIELD_UNIX_SECS | FT_XFIELD_UNIX_NSECS | FT_XFIELD_SYSUPTIME |
        FT_XFIELD_TCP_FLAGS | FT_XFIELD_PROT)) 
    {
        fterr_warnx("Flow record missing required field for format.");
        return -1;
    }

    ftio_get_ver(ftinp, &ftv);
    fts3rec_compute_offsets(&fo, &ftv);

    while ((rec = (char*)ftio_read(ftinp))) 
    {
        nflows++;
        cur.unix_secs = ((u_int32*)(rec+fo.unix_secs));
        cur.unix_nsecs = ((u_int32*)(rec+fo.unix_nsecs));
        cur.sysUpTime = ((u_int32*)(rec+fo.sysUpTime));
        cur.dOctets = ((u_int32*)(rec+fo.dOctets));
        cur.dPkts = ((u_int32*)(rec+fo.dPkts));
        cur.First = ((u_int32*)(rec+fo.First));
        cur.Last = ((u_int32*)(rec+fo.Last));
        cur.srcaddr = ((u_int32*)(rec+fo.srcaddr));
        cur.dstaddr = ((u_int32*)(rec+fo.dstaddr));
        cur.srcport = ((u_int16*)(rec+fo.srcport));
        cur.dstport = ((u_int16*)(rec+fo.dstport));
        cur.prot = ((u_int8*)(rec+fo.prot));
        cur.tcp_flags = ((u_int8*)(rec+fo.tcp_flags));

        if (*cur.prot != IPPROTO_TCP)
            continue;

        ftt_begin = ftltime(*cur.sysUpTime, *cur.unix_secs, *cur.unix_nsecs, *cur.First);
        ftt_end = ftltime(*cur.sysUpTime, *cur.unix_secs, *cur.unix_nsecs, *cur.Last);

        double begin = double(ftt_begin.secs) + double(ftt_begin.msecs) / 1000.0;
        double end = double(ftt_end.secs) + double(ftt_end.msecs) / 1000.0;


#if 0
        std::cout << *cur.srcaddr << ' ' << *cur.dstaddr << ' ' << *cur.srcport << ' ' << *cur.dstport << ' ' << int(*cur.prot) << ' ' << *cur.dPkts << ' ' << *cur.dOctets << ' ' << std::setprecision(15) << begin << ' ' << std::setprecision(15) << end << ' ' << std::setprecision(6) << (end-begin) << std::endl;
#endif

        tottcpflows++;

        min_begin = std::min(begin, min_begin);

        FlowBlock fb(cur, begin, end);
        flows.push_back(fb);
   }
   return nflows;
}
#endif // HAVE_FLOWTOOLS


int netflow_decode(void *voidp, std::list<FlowBlock> &flows, 
                   int &tottcpflows, double &min_begin)
{
    FILE *infile = (FILE *)voidp;

    int nflows = 0;

    // first 24 bytes: header
    NetflowV5Header fh;
    if (fread(&fh, sizeof(NetflowV5Header), 1, infile) != 1)
	    return -1;

#if 0
    std::cout << "flow block count: " << ntohs(fh.flow_count) << std::endl;
#endif

    // 48 bytes flow record
    NetflowV5Record fr;
    for (int i = 0; i < ntohs(fh.flow_count); ++i)
    {
        if (fread(&fr, sizeof(NetflowV5Record), 1, infile) != 1)
            return nflows;
        nflows++;

        //
        // only accept TCP flows, for now
        //
        if (fr.proto != IPPROTO_TCP)
            continue;

        double begin = double(fr.flowbegin) / 1000.0;
        double end = double(fr.flowend) / 1000.0;

#if 0
        std::cout << fr.srcaddr << ' ' << fr.dstaddr << ' ' << fr.srcport << ' ' << fr.dstport << ' ' << int(fr.proto) << ' ' << fr.npkts << ' ' << fr.noctets << ' ' << std::setprecision(15) << begin << ' ' << std::setprecision(15) << end << ' ' << std::setprecision(6) << (end-begin) << std::endl;
#endif

        tottcpflows++;

        min_begin = std::min(begin, min_begin);

        FlowBlock fb(fr, begin, end);
        flows.push_back(fb);
    }
    return nflows;
}


void flow_dump(std::ostream &os, FlowDescript &fd, const char *tag)
{
    int payload = fd.m_octets - fd.m_pkts * 40;
    if (payload < 100)
        return;

    // os << '<' << tag << ' ' << pp << ' ' << fd << '>';
    os << '<' << tag << ' ' << fd << '>';
}


bool add_flow(std::list<FlowDescript> *fdlist, FlowDescript &fd)
{
    // since flow records are sorted on input (or as intermediate step
    // prior to organize()), we only need to check last record in list

    FDITER last_rec = fdlist->end();
    last_rec--;
    bool surgery = false;

    // epsilon to consider flows to be the same is implementation dependent...
    if ((fabs((last_rec->m_start + last_rec->m_duration) - fd.m_start) < 0.01)
        && (last_rec->m_tcpflags & TH_SYN &&
            !(last_rec->m_tcpflags & TH_FIN ||
              last_rec->m_tcpflags & TH_RST) &&
            !fd.m_tcpflags & TH_SYN))
    {
        last_rec->accumulate(fd);
        surgery = true;
    }
    else
    {
        fdlist->push_back(fd);
    }
    return (surgery);
}


void organize_flows(std::ostream &os, FlowBlock &fb, std::set<HostPair> &hostpairs, int &surgery)
{
    double begin = fb.flowbegin;
    double end = fb.flowend;
    double duration = end - begin;

    HostPair hp(fb.srcaddr, fb.dstaddr);
    PortPair pp(fb.srcport, fb.dstport, fb.proto);
    FlowDescript fd(fb.npkts, fb.noctets, begin, duration, fb.tcp_flags, fb.srcport, fb.dstport, fb.proto);
 
    std::set<PortPair> *pplist = 0;
    std::list<FlowDescript> *fdlist = 0;

    HPITER it = hostpairs.find(hp);
    if (it != hostpairs.end())
    {
        pplist = it->m_portpairs;
    }
    else
    {
        pplist = new std::set<PortPair>();
        hp.m_portpairs = pplist;
        hostpairs.insert(hp);
    }

    PPITER ppi = pplist->find(pp);
    if (ppi != pplist->end())
    {
        PortPair ppe = *ppi;
        fdlist = ppe.m_flows;
    }
    else
    {
        fdlist = new std::list<FlowDescript>();
        pp.m_flows = fdlist;    
        pplist->insert(pp);
    }

    // add this flow descriptor to list 
    surgery += add_flow(fdlist, fd);
}


void usage(const char *proggie)
{
    std::cerr << "usage: " << proggie << " [-i <maxinterconn>] [-n] [-w]" << std::endl;
    std::cerr << "  takes stdin, produces stdout" << std::endl;
    std::cerr << std::endl;
    std::cerr << "  -i<int>  specify maximum inter-connection time" << std::endl;
    std::cerr << "           (default: " << USER_IDLE_MAX << " seconds)" << std::endl;
    std::cerr << "  -n       netflow records as input (default is flowtools fmt - if available)" << std::endl;
    std::cerr << "  -w       relax constraint of only dumping 'well-formed' TCP "
              << std::endl;
    std::cerr << "           flows (SYN and FIN or RST)" << std::endl;
}


int main(int argc, char **argv)
{
    int totflows = 0;
    int tottcpflows = 0;
    int totwftcpflows = 0;
    int surgery = 0;

    // only dump stuff for well-formed flows
    bool only_wf = true;

#if HAVE_FLOWTOOLS
    struct ftprof ftp;
    struct ftio ftinp;
    bool netflow = false;
#else
    bool netflow = true;
#endif

    int c;
    while ((c = getopt(argc, argv, "i:wn")) != EOF)
    {
        switch (c)
        {
            case 'i':
                USER_IDLE_MAX = atof(optarg);
                break; 
            case 'n':
                netflow = true;
                break; 
            case 'w':
                only_wf = false;
                break;
            default:
                usage(argv[0]);
                exit(0);
        }
    }

#if HAVE_FLOWTOOLS
    if (!netflow)
    {
        fterr_setid(argv[0]);
        ftprof_start(&ftp);
        if (ftio_init(&ftinp, 0, FT_IO_FLAG_READ) < 0)
            fterr_errx(1, "ftio_init(): failed");
    }
#endif

    double first_record = __DBL_MAX__;
    std::list<FlowBlock> flows;
    while (1)
    {
        int f = 0;
        if (netflow)
            f = netflow_decode(stdin, flows, tottcpflows, first_record);
#if HAVE_FLOWTOOLS
        else
            f = ftools_decode(&ftinp, flows, tottcpflows, first_record);
#endif

        if (f <= 0)
            break;
        totflows += f;
    }	    


    std::cerr << "sorting tcp flow records... " << std::flush;
    time_t sortbegin = time(NULL);
    flows.sort();
    std::cerr << " took " << (time(NULL) - sortbegin) << " sec." << std::endl;


    // for now, send all output to stdout
    std::ostream &os = std::cout;

    os << "first-record " << std::setprecision(15) << first_record << std::endl;

    // take raw flow list and organize into host pairs.  do surgery, etc.
    std::set<HostPair> hostlist;
    for (std::list<FlowBlock>::iterator fbiter = flows.begin();
         fbiter != flows.end(); ++fbiter)
    {
        organize_flows(os, *fbiter, hostlist, surgery);
    }


    // output stuff
    for (HPITER hpit = hostlist.begin(); hpit != hostlist.end(); ++hpit)
    {
        HostPair hp = *hpit;
        std::set<PortPair> *pplist = hp.m_portpairs;
        std::cout << hp << ' ';


        std::list<FlowDescript> newlist;

        for (PPITER ppit = pplist->begin(); ppit != pplist->end(); ++ppit)
        {
            PortPair ppx = *ppit;
            std::list<FlowDescript> *fdlist = ppx.m_flows;
            for (FDITER fdi = fdlist->begin(); fdi != fdlist->end(); ++fdi)
            {
                newlist.push_back(*fdi);
            }
            delete fdlist;
        }

        newlist.sort();
        // totwftcpflows += flow_output(os, only_wf, ppx, fdlist);

        FDITER fdi = newlist.begin();
        double prev_conn = fdi->m_start;
        while (fdi != newlist.end())
        {
            if ((fdi->m_start - prev_conn) > USER_IDLE_MAX)
            {
                std::cout << std::endl << hp << ' ';
            }

            if ((*fdi).m_tcpflags & TH_SYN &&
                ((*fdi).m_tcpflags & TH_FIN || 
                (*fdi).m_tcpflags & TH_RST))
            {
                flow_dump(os, *fdi, "SF/R");
                totwftcpflows++;
                prev_conn = fdi->m_start;
            }
            else
            {
                if (!only_wf)
                {
                    flow_dump(os, *fdi, "!WF");
                    prev_conn = fdi->m_start;
                }
            }
            fdi++;
        } 
        newlist.clear();
        std::cout << std::endl;
    }

    std::cerr << "total flows: " << totflows << std::endl;
    std::cerr << "total TCP flows: " << tottcpflows << std::endl;
    std::cerr << "total well-formed TCP flows: " << totwftcpflows << std::endl;
    std::cerr << "surgery performed: " << surgery << std::endl;

    exit(0);
}

