#include "ping.h"
struct proto proto_v4 = {
    proc_v4, send_v4, NULL, NULL, 0, IPPROTO_ICMP
};

#ifdef IPV6
struct proto proto_v6 = {
    proc_v6, send_v6, NULL, NULL, 0, IPPROTO_ICMPV6
};
#endif

int datalen = 56;   /* data that goes with ICMP echo request */
const char *usage ="\
Usage: ping [-b broadcast] [-c count] [-d so_debug] [-f flood]\n\
            [-h help] [-i interval] [-q quiet] [-s packetsize]\n\
            [-t ttl] [-v verbose]\
            ";


int main(int argc, char **argv){
    int c;
    int i;
    ping_interval = 1;

    opterr = 0; /* don't want getopt() writing to stderr */
    while((c = getopt(argc, argv, "bc:dfhi:qs:t:v")) != -1){
        switch (c){
            case 'b':
                broadcast_flag = 1;
                break;
            case 'c':
                count_flag = 1;
                ncount = atoi(optarg);
                break;
            case 'd':
                sodebug_flag = 1;
                break;
            case 'f':
                flood_flag = 1;
                setbuf(stdout, NULL);
                break;
            case 'h':
                puts(usage);
                return 0;
            case 'i':
                ping_interval = atof(optarg);
                if(ping_interval <= 0)
                    err_quit("ping: bad timing interval\n");
                interval_flag = 1;
                break;
            case 'q':
                quiet_flag = 1;
                break;
            case 's':
                datalen = atof(optarg);
                if(datalen < 56)
                    err_quit("byte size %d out of range\n", datalen);
                break;
            case 't':
                ttl_flag = 1;
                i = atoi(optarg);
                if(i < 0 || i > 255)
                    err_quit("tll %u out of range\n", i);
                ttl = i;
                break;
            case 'v':
                verbose = 1;
                break;
            case '?':
                err_quit("unrecognized option: %c", c);
        }
    }

    packetTransmittedNum = 0;
    packetReceivedNum = 0;
    packetDupNum = 0;
    nrtt = 0;
    memset(duparr, 0, sizeof(duparr));

    if (optind != argc-1)
        err_quit(usage);
    host = argv[optind];

    pid = getpid() & 0xffff;
    signal(SIGALRM, sig_alrm);
    signal(SIGINT, interrupt_event);

    ai = host_serv(host, NULL, 0, 0);

    /* check broadcast address */
    if(!broadcast_flag){
        int j;
        for(j = 16; j > 0; j--){
            if(Sock_ntop_host(ai->ai_addr, ai->ai_addrlen)[j] == '\0'){
                if(Sock_ntop_host(ai->ai_addr, ai->ai_addrlen)[j-3] == '2' &&
                        Sock_ntop_host(ai->ai_addr, ai->ai_addrlen)[j-2] == '5' &&
                        Sock_ntop_host(ai->ai_addr, ai->ai_addrlen)[j-1] == '5' )
                    err_quit("Do you want to ping broadcast? Then -b\n");
            }
            else
                break;
        }
    }

    printf("PING %s (%s): %d bytes of data.\n", ai->ai_canonname,
            Sock_ntop_host(ai->ai_addr, ai->ai_addrlen), datalen);

    /* initialize according to protocol */
    if (ai->ai_family == AF_INET){
        pr = &proto_v4;
#ifdef IPV6
    } else if (ai->ai_family == AF_INET6){
        pr = &proto_v6;
        if (IN6_IS_ADDR_V4MAPPED(&(((struct sockaddr_in6 *)
                            ai->ai_addr)->sin6_addr)))
            err_quit("cannot ping IPv4-mapped IPv6 address");
#endif
    } else
        err_quit("unknown address family %d", ai->ai_family);

    pr->sasend = ai->ai_addr;
    pr->sarecv = calloc(1, ai->ai_addrlen);
    pr->salen = ai->ai_addrlen;

    readloop();

    exit(0);
}

void proc_v4(char *ptr, ssize_t len, struct timeval *tvrecv){
    int hlen1, icmplen;
    double rtt;
    struct ip *ip;
    struct icmp *icmp;
    struct timeval *tvsend;

    ip = (struct ip *) ptr; /* start of IP header */
    hlen1 = ip->ip_hl << 2; /* length of IP header */

    icmp = (struct icmp *) (ptr + hlen1); /* start of ICMP header */
    if ( (icmplen = len - hlen1) < 8)
        err_quit("icmplen (%d) < 8", icmplen);

    if (icmp->icmp_type == ICMP_ECHOREPLY) {
        if (icmp->icmp_id != pid){
            return; /* not a response to our ECHO_REQUEST */
        }
        if (icmplen < 16)
            err_quit("icmplen (%d) < 16", icmplen);

        packetReceivedNum++;
        tvsend = (struct timeval *) icmp->icmp_data;
        tv_sub(tvrecv, tvsend);
        rtt = tvrecv->tv_sec * 1000.0 + tvrecv->tv_usec / 1000.0;
        if(rtt < rtt_min)
            rtt_min = rtt;
        if(rtt > rtt_max)
            rtt_max = rtt;
        rtt_sum += rtt;
        rtt_sum1 += rtt*rtt;
        nrtt++;

        if(duparr[icmp->icmp_seq] == 1){
            packetReceivedNum--;
            dup_flag = 1;
            packetDupNum++;
        }
        else{
            duparr[icmp->icmp_seq] = 1;
            dup_flag = 0;
        }


        if(!quiet_flag && !flood_flag){
            printf("%d bytes from %s: seq=%u, ttl=%d, rtt=%.3f ms",
                    icmplen, Sock_ntop_host(pr->sarecv, pr->salen),
                    icmp->icmp_seq, ip->ip_ttl, rtt);

            if(dup_flag)
                printf(" (DUP!)");

            printf("\n");
        }
        if(flood_flag){
            write(STDOUT_FILENO, &BSPACE, 1);
        }

        if(count_flag){
            if(packetTransmittedNum >= ncount)
                interrupt_event();
        }

    } else if (icmp->icmp_type == ICMP_TIME_EXCEEDED) {
            printf("From %s seq=%u Time to live exceeded\n",
                    Sock_ntop_host(pr->sarecv, pr->salen),
                    nsent-1);
    } else if (verbose){
        printf("  %d bytes from %s: type = %d, code = %d\n",
                icmplen, Sock_ntop_host(pr->sarecv, pr->salen),
                icmp->icmp_type, icmp->icmp_code);
    }
}

void proc_v6(char *ptr, ssize_t len, struct timeval* tvrecv){
#ifdef IPV6
    int hlen1, icmp6len;
    double rtt;
    struct ip6_hdr *ip6;
    struct icmp6_hdr *icmp6;
    struct timeval *tvsend;

    /*
       ip6 = (struct ip6_hdr *) ptr; // start of IPv6 header 
       hlen1 = sizeof(struct ip6_hdr);
       if (ip6->ip6_nxt != IPPROTO_ICMPV6)
       err_quit("next header not IPPROTO_ICMPV6");

       icmp6 = (struct icmp6_hdr *) (ptr + hlen1);
       if ( (icmp6len = len - hlen1) < 8)
       err_quit("icmp6len (%d) < 8", icmp6len);
       */

    icmp6=(struct icmp6_hdr *)ptr;  
    if((icmp6len=len)<8)                    //len-40
        err_quit("icmp6len (%d) < 8", icmp6len);


    if (icmp6->icmp6_type == ICMP6_ECHO_REPLY) {
        if (icmp6->icmp6_id != pid)
            return; /* not a response to our ECHO_REQUEST */
        if (icmp6len < 16)
            err_quit("icmp6len (%d) < 16", icmp6len);

        tvsend = (struct timeval *) (icmp6 + 1);
        tv_sub(tvrecv, tvsend);
        rtt = tvrecv->tv_sec * 1000.0 + tvrecv->tv_usec / 1000.0;
        if(rtt > rtt_min)
            rtt_min = rtt;
        if(rtt < rtt_max)
            rtt_max = rtt;
        rtt_sum += rtt;

        printf("%d bytes from %s: seq=%u, hlim=%d, rtt=%.3f ms\n",
                icmp6len, Sock_ntop_host(pr->sarecv, pr->salen),
                icmp6->icmp6_seq, ip6->ip6_hlim, rtt);

    } else if (verbose) {
        printf("  %d bytes from %s: type = %d, code = %d\n",
                icmp6len, Sock_ntop_host(pr->sarecv, pr->salen),
                icmp6->icmp6_type, icmp6->icmp6_code);
    }
#endif /* IPV6 */
}

unsigned short in_cksum(unsigned short *addr, int len){
    int nleft = len;
    int sum = 0;
    unsigned short *w = addr;
    unsigned short answer = 0;

    /*
     * Our algorithm is simple, using a 32 bit accumulator (sum), we add
     * sequential 16 bit words to it, and at the end, fold back all the
     * carry bits from the top 16 bits into the lower 16 bits.
     */
    while (nleft > 1){
        sum += *w++;
        nleft -= 2;
    }

    /* 4mop up an odd byte, if necessary */
    if (nleft == 1){
        *(unsigned char *)(&answer) = *(unsigned char *)w ;
        sum += answer;
    }

    /* 4add back carry outs from top 16 bits to low 16 bits */
    sum = (sum >> 16) + (sum & 0xffff);  /* add hi 16 to low 16 */
    sum += (sum >> 16);             /* add carry */
    answer = ~sum;                  /* truncate to 16 bits */
    return(answer);
}

void send_v4(void){
    int len;
    struct icmp *icmp;



    icmp = (struct icmp *) sendbuf;
    icmp->icmp_type = ICMP_ECHO;
    icmp->icmp_code = 0;
    icmp->icmp_id = pid;
    icmp->icmp_seq = nsent++;
    gettimeofday((struct timeval *) icmp->icmp_data, NULL);

    len = 8 + datalen;   /* checksum ICMP header and data */
    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = in_cksum((u_short *) icmp, len);

    sendto(sockfd, sendbuf, len, 0, pr->sasend, pr->salen);
    packetTransmittedNum++;
    if(nsent > MAX_DUP_CHK)
        interrupt_event();

    if(!quiet_flag && flood_flag){
        write(STDOUT_FILENO, &DOT, 1);
    }

    if(count_flag)
        if(packetTransmittedNum >= ncount)
            return;
}

void send_v6(){
#ifdef IPV6
    int len;
    struct icmp6_hdr *icmp6;

    icmp6 = (struct icmp6_hdr *) sendbuf;
    icmp6->icmp6_type = ICMP6_ECHO_REQUEST;
    icmp6->icmp6_code = 0;
    icmp6->icmp6_id = pid;
    icmp6->icmp6_seq = nsent++;
    gettimeofday((struct timeval *) (icmp6 + 1), NULL);

    len = 8 + datalen; /* 8-byte ICMPv6 header */

    sendto(sockfd, sendbuf, len, 0, pr->sasend, pr->salen);
    /* kernel calculates and stores checksum for us */
#endif /* IPV6 */
}

void readloop(void){
    int size;
    char recvbuf[BUFSIZE];
    socklen_t len;
    ssize_t n;
    struct timeval tval;


    timePast = 0;
    rtt_min = 99999999;
    rtt_max = 0;
    rtt_sum = 0;
    rtt_sum1 = 0;


    if((sockfd = socket(pr->sasend->sa_family, SOCK_RAW, pr->icmpproto)) < 0)
        err_quit("socket error");

    setuid(getuid()); /* don't need special permissions any more */

    size = 60 * 1024; /* OK if setsockopt fails */
    if(sodebug_flag)
        setsockopt(sockfd, SOL_SOCKET, SO_DEBUG, &size, sizeof(size));
    //setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    if(ttl_flag){
        if(setsockopt(sockfd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) == -1)
            err_quit("cannot set multicast ttl\n");
        //else
            //printf("set ttl to %d\n", ttl);
    }
    if(broadcast_flag){
        if(setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, (char *)&size, sizeof(size)) == -1)
            err_quit("cannot set broadcast\n");
        //else
            //printf("set broadcast successfully\n");
    }


    sig_alrm(SIGALRM); /* send first packet */


    for ( ; ; ){
        len = pr->salen;
        n = recvfrom(sockfd, recvbuf, sizeof(recvbuf), 0, pr->sarecv, &len);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            else
                err_sys("recvfrom error");
        }

        gettimeofday(&tval, NULL);
        (*pr->fproc)(recvbuf, n, &tval);



    }
}

void sig_alrm(int signo){
    (*pr->fsend)();

    if(ping_interval >= 1)
        alarm(ping_interval);
    else
        ualarm((useconds_t)(ping_interval*1000000), (useconds_t)(ping_interval*1000000));
    return;         /* probably interrupts recvfrom() */
}

void tv_sub(struct timeval *out, struct timeval *in){
    if ( (out->tv_usec -= in->tv_usec) < 0){  /* out -= in */
        --out->tv_sec;
        out->tv_usec += 1000000;
    }
    out->tv_sec -= in->tv_sec;
}




char * sock_ntop_host(const struct sockaddr *sa, socklen_t salen){
    static char str[128];     /* Unix domain is largest */

    switch (sa->sa_family){
        case AF_INET:{
                         struct sockaddr_in *sin = (struct sockaddr_in *) sa;

                         if (inet_ntop(AF_INET, &sin->sin_addr, str, sizeof(str)) == NULL)
                             return(NULL);
                         return(str);
                     }

#ifdef  IPV6
        case AF_INET6: {
                           struct sockaddr_in6     *sin6 = (struct sockaddr_in6 *) sa;

                           if (inet_ntop(AF_INET6, &sin6->sin6_addr, str, sizeof(str)) == NULL)
                               return(NULL);
                           return(str);
                       }
#endif

#ifdef  HAVE_SOCKADDR_DL_STRUCT
        case AF_LINK: {
                          struct sockaddr_dl      *sdl = (struct sockaddr_dl *) sa;

                          if (sdl->sdl_nlen > 0)
                              snprintf(str, sizeof(str), "%*s",
                                      sdl->sdl_nlen, &sdl->sdl_data[0]);
                          else
                              snprintf(str, sizeof(str), "AF_LINK, index=%d", sdl->sdl_index);
                          return(str);
                      }
#endif
        default:
                      snprintf(str, sizeof(str), "sock_ntop_host: unknown AF_xxx: %d, len %d",
                              sa->sa_family, salen);
                      return(str);
    }
    return (NULL);
}

char * Sock_ntop_host(const struct sockaddr *sa, socklen_t salen){
    char *ptr;

    if ( (ptr = sock_ntop_host(sa, salen)) == NULL)
        err_sys("sock_ntop_host error");        /* inet_ntop() sets errno */
    return(ptr);
}

struct addrinfo * host_serv(const char *host, const char *serv, int family, int socktype){
    int n;
    struct addrinfo hints, *res;

    bzero(&hints, sizeof(struct addrinfo));
    hints.ai_flags = AI_CANONNAME;  /*always return canonical name */
    hints.ai_family = family;       /* AF_UNSPEC, AF_INET, AF_INET6, etc. */
    hints.ai_socktype = socktype;   /* 0, SOCK_STREAM, SOCK_DGRAM, etc. */

    if ( (n = getaddrinfo(host, serv, &hints, &res)) != 0)
        return(NULL);

    return(res);    /* return pointer to first on linked list */
}
/* end host_serv */

static void err_doit(int errnoflag, int level, const char *fmt, va_list ap){
    int errno_save, n;
    char buf[MAXLINE];

    errno_save = errno;     /* value caller might want printed */
#ifdef  HAVE_VSNPRINTF
    vsnprintf(buf, sizeof(buf), fmt, ap);   /* this is safe */
#else
    vsprintf(buf, fmt, ap);             /* this is not safe */
#endif
    n = strlen(buf);
    if (errnoflag)
        snprintf(buf+n, sizeof(buf)-n, ": %s", strerror(errno_save));
    strcat(buf, "\n");

    if (daemon_proc) {
        syslog(level, buf);
    } else {
        fflush(stdout); /*in case stdout and stderr are the same */
        fputs(buf, stderr);
        fflush(stderr);
    }
    return;
}


/* Fatal error unrelated to a system call.
 * Print a message and terminate. */

void err_quit(const char *fmt, ...){
    va_list ap;

    va_start(ap, fmt);
    err_doit(0, LOG_ERR, fmt, ap);
    va_end(ap);
    exit(1);
}

/* Fatal error related to a system call.
 * Print a message and terminate. */

void err_sys(const char *fmt, ...){
    va_list ap;

    va_start(ap, fmt);
    err_doit(1, LOG_ERR, fmt, ap);
    va_end(ap);
    exit(1);
}

void interrupt_event(){
    packetLossNum = packetTransmittedNum-packetReceivedNum;
    if(packetTransmittedNum == 0)
        packetTransmittedNum = 1;
    if(nrtt == 0)
        nrtt = 1;
    rtt_sum /= (double)nrtt;  //it became rtt_avg now
    rtt_sum1 /= (double)nrtt;
    rtt_mdev = sqrt(rtt_sum1 - rtt_sum*rtt_sum);

    printf("\n--- %s ping statistics ---\n", ai->ai_canonname);
    printf("%d packets transmitted, %d received, ", packetTransmittedNum, packetReceivedNum);
    if(packetDupNum > 0)
        printf("+%d duplicates, ", packetDupNum);
    printf("%d%% packet loss\n", packetLossNum*100/packetTransmittedNum);
    if(packetReceivedNum > 0)
        printf("rtt min/avg/max/mdev = %.3f/%.3f/%.3f/%.3f ms\n", \
                rtt_min, rtt_sum, rtt_max, rtt_mdev);
    else
        printf("\n");
    close(sockfd);
    exit(1);
}

