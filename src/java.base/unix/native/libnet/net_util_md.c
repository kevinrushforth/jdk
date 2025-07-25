/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
#include <dlfcn.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/tcp.h> // defines TCP_NODELAY
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if defined(__linux__)
#include <arpa/inet.h>
#include <net/route.h>
#include <sys/utsname.h>
#endif

#if defined(MACOSX)
#include <sys/sysctl.h>
#endif

#include "jvm.h"
#include "net_util.h"

#include "java_net_SocketOptions.h"
#include "java_net_InetAddress.h"

#if defined(__linux__) && !defined(IPV6_FLOWINFO_SEND)
#define IPV6_FLOWINFO_SEND      33
#endif

/* Perform platform specific initialization.
 * Returns 0 on success, non-0 on failure */
int
NET_PlatformInit()
{
    // Not needed on unix
    return 0;
}

void
NET_ThrowByNameWithLastError(JNIEnv *env, const char *name,
                   const char *defaultDetail) {
    JNU_ThrowByNameWithMessageAndLastError(env, name, defaultDetail);
}

void
NET_ThrowNew(JNIEnv *env, int errorNumber, char *msg) {
    char fullMsg[512];
    if (!msg) {
        msg = "no further information";
    }
    switch(errorNumber) {
    case EBADF:
        jio_snprintf(fullMsg, sizeof(fullMsg), "socket closed: %s", msg);
        JNU_ThrowByName(env, JNU_JAVANETPKG "SocketException", fullMsg);
        break;
    case EINTR:
        JNU_ThrowByName(env, JNU_JAVAIOPKG "InterruptedIOException", msg);
        break;
    default:
        errno = errorNumber;
        JNU_ThrowByNameWithLastError(env, JNU_JAVANETPKG "SocketException", msg);
        break;
    }
}

jint  IPv4_supported()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0) ;
    if (fd < 0) {
        return JNI_FALSE;
    }
    close(fd);
    return JNI_TRUE;
}

#if defined(DONT_ENABLE_IPV6)
jint  IPv6_supported()
{
    return JNI_FALSE;
}

#else /* !DONT_ENABLE_IPV6 */

jint  IPv6_supported()
{
    int fd;
    void *ipv6_fn;
    SOCKETADDRESS sa;
    socklen_t sa_len = sizeof(SOCKETADDRESS);

    fd = socket(AF_INET6, SOCK_STREAM, 0) ;
    if (fd < 0) {
        /*
         *  TODO: We really can't tell since it may be an unrelated error
         *  for now we will assume that AF_INET6 is not available
         */
        return JNI_FALSE;
    }
    close(fd);

    /**
     * Linux - check if any interface has an IPv6 address.
     * Don't need to parse the line - we just need an indication.
     */
#ifdef __linux__
    {
        FILE *fP = fopen("/proc/net/if_inet6", "r");
        char buf[255];
        char *bufP;

        if (fP == NULL) {
            return JNI_FALSE;
        }
        bufP = fgets(buf, sizeof(buf), fP);
        fclose(fP);
        if (bufP == NULL) {
            return JNI_FALSE;
        }
    }
#endif

    /*
     *  OK we may have the stack available in the kernel,
     *  we should also check if the APIs are available.
     */
    ipv6_fn = JVM_FindLibraryEntry(RTLD_DEFAULT, "inet_pton");
    if (ipv6_fn == NULL ) {
        return JNI_FALSE;
    } else {
        return JNI_TRUE;
    }
}
#endif /* DONT_ENABLE_IPV6 */

jint reuseport_supported(int ipv6_available)
{
    /* Do a simple dummy call, and try to figure out from that */
    int one = 1;
    int rv, s;
    if (ipv6_available) {
        s = socket(PF_INET6, SOCK_STREAM, 0);
    } else {
        s = socket(PF_INET, SOCK_STREAM, 0);
    }
    if (s < 0) {
        return JNI_FALSE;
    }
    rv = setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (void *)&one, sizeof(one));
    if (rv != 0) {
        rv = JNI_FALSE;
    } else {
        rv = JNI_TRUE;
    }
    close(s);
    return rv;
}

void NET_ThrowUnknownHostExceptionWithGaiError(JNIEnv *env,
                                               const char* hostname,
                                               int gai_error)
{
    int size;
    char *buf;
    const char *error_string = gai_strerror(gai_error);
    if (error_string == NULL)
        error_string = "unknown error";
    int enhancedExceptions = getEnhancedExceptionsAllowed(env);
    if (enhancedExceptions == ENH_INIT_ERROR && (*env)->ExceptionCheck(env)) {
        return;
    }

    if (enhancedExceptions == ENH_ENABLED) {
        size = strlen(hostname);
    } else {
        size = 0;
    }
    size += strlen(error_string) + 3;

    buf = (char *) malloc(size);
    if (buf) {
        jstring s;
        int n;
        if (enhancedExceptions == ENH_ENABLED) {
            n = snprintf(buf, size, "%s: %s", hostname, error_string);
        } else {
            n = snprintf(buf, size, " %s", error_string);
        }
        if (n >= 0) {
            s = JNU_NewStringPlatform(env, buf);
            if (s != NULL) {
                jobject x = JNU_NewObjectByName(env,
                                            "java/net/UnknownHostException",
                                            "(Ljava/lang/String;)V", s);
                if (x != NULL)
                    (*env)->Throw(env, x);
            }
        }
        free(buf);
    }
}

JNIEXPORT jint JNICALL
NET_EnableFastTcpLoopback(int fd) {
    return 0;
}

/**
 * See net_util.h for documentation
 */
JNIEXPORT int JNICALL
NET_InetAddressToSockaddr(JNIEnv *env, jobject iaObj, int port,
                          SOCKETADDRESS *sa, int *len,
                          jboolean v4MappedAddress)
{
    jint family = getInetAddress_family(env, iaObj);
    JNU_CHECK_EXCEPTION_RETURN(env, -1);
    memset((char *)sa, 0, sizeof(SOCKETADDRESS));

    if (ipv6_available() &&
        !(family == java_net_InetAddress_IPv4 &&
          v4MappedAddress == JNI_FALSE))
    {
        jbyte caddr[16];
        jint address;

        if (family == java_net_InetAddress_IPv4) {
            // convert to IPv4-mapped address
            memset((char *)caddr, 0, 16);
            address = getInetAddress_addr(env, iaObj);
            JNU_CHECK_EXCEPTION_RETURN(env, -1);
            if (address == INADDR_ANY) {
                /* we would always prefer IPv6 wildcard address
                 * caddr[10] = 0xff;
                 * caddr[11] = 0xff; */
            } else {
                caddr[10] = 0xff;
                caddr[11] = 0xff;
                caddr[12] = ((address >> 24) & 0xff);
                caddr[13] = ((address >> 16) & 0xff);
                caddr[14] = ((address >> 8) & 0xff);
                caddr[15] = (address & 0xff);
            }
        } else {
            getInet6Address_ipaddress(env, iaObj, (char *)caddr);
        }
        sa->sa6.sin6_port = htons(port);
        memcpy((void *)&sa->sa6.sin6_addr, caddr, sizeof(struct in6_addr));
        sa->sa6.sin6_family = AF_INET6;
        if (len != NULL) {
            *len = sizeof(struct sockaddr_in6);
        }

        /* handle scope_id */
        if (family != java_net_InetAddress_IPv4) {
            if (ia6_scopeidID) {
                sa->sa6.sin6_scope_id = getInet6Address_scopeid(env, iaObj);
            }
        }
    } else {
        jint address;
        if (family != java_net_InetAddress_IPv4) {
            JNU_ThrowByName(env, JNU_JAVANETPKG "SocketException", "Protocol family unavailable");
            return -1;
        }
        address = getInetAddress_addr(env, iaObj);
        JNU_CHECK_EXCEPTION_RETURN(env, -1);
        sa->sa4.sin_port = htons(port);
        sa->sa4.sin_addr.s_addr = htonl(address);
        sa->sa4.sin_family = AF_INET;
        if (len != NULL) {
            *len = sizeof(struct sockaddr_in);
        }
    }
    return 0;
}

int
NET_IsIPv4Mapped(jbyte* caddr) {
    int i;
    for (i = 0; i < 10; i++) {
        if (caddr[i] != 0x00) {
            return 0; /* false */
        }
    }

    if (((caddr[10] & 0xff) == 0xff) && ((caddr[11] & 0xff) == 0xff)) {
        return 1; /* true */
    }
    return 0; /* false */
}

int
NET_IPv4MappedToIPv4(jbyte* caddr) {
    return ((caddr[12] & 0xff) << 24) | ((caddr[13] & 0xff) << 16) | ((caddr[14] & 0xff) << 8)
        | (caddr[15] & 0xff);
}

int
NET_IsEqual(jbyte* caddr1, jbyte* caddr2) {
    int i;
    for (i = 0; i < 16; i++) {
        if (caddr1[i] != caddr2[i]) {
            return 0; /* false */
        }
    }
    return 1;
}

int NET_IsZeroAddr(jbyte* caddr) {
    int i;
    for (i = 0; i < 16; i++) {
        if (caddr[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/*
 * Wrapper for getsockopt system routine - does any necessary
 * pre/post processing to deal with OS specific oddities :-
 *
 * On Linux the SO_SNDBUF/SO_RCVBUF values must be post-processed
 * to compensate for an incorrect value returned by the kernel.
 */
int
NET_GetSockOpt(int fd, int level, int opt, void *result,
               int *len)
{
    int rv;
    socklen_t socklen = *len;

    rv = getsockopt(fd, level, opt, result, &socklen);
    *len = socklen;

    if (rv < 0) {
        return rv;
    }

#ifdef __linux__
    /*
     * On Linux SO_SNDBUF/SO_RCVBUF aren't symmetric. This
     * stems from additional socket structures in the send
     * and receive buffers.
     */
    if ((level == SOL_SOCKET) && ((opt == SO_SNDBUF)
                                  || (opt == SO_RCVBUF))) {
        int n = *((int *)result);
        n /= 2;
        *((int *)result) = n;
    }
#endif

/* Workaround for Mac OS treating linger value as
 *  signed integer
 */
#ifdef MACOSX
    if (level == SOL_SOCKET && opt == SO_LINGER) {
        struct linger* to_cast = (struct linger*)result;
        to_cast->l_linger = (unsigned short)to_cast->l_linger;
    }
#endif
    return rv;
}

/*
 * Wrapper for setsockopt system routine - performs any
 * necessary pre/post processing to deal with OS specific
 * issue :-
 *
 * On Solaris need to limit the suggested value for SO_SNDBUF
 * and SO_RCVBUF to the kernel configured limit
 *
 * For IP_TOS socket option need to mask off bits as this
 * aren't automatically masked by the kernel and results in
 * an error.
 */
int
NET_SetSockOpt(int fd, int level, int  opt, const void *arg,
               int len)
{

#ifndef IPTOS_TOS_MASK
#define IPTOS_TOS_MASK 0x1e
#endif
#ifndef IPTOS_PREC_MASK
#define IPTOS_PREC_MASK 0xe0
#endif

#if defined(_ALLBSD_SOURCE)
#if defined(KIPC_MAXSOCKBUF)
    int mib[3];
    size_t rlen;
#endif

    int *bufsize;

#ifdef __APPLE__
    static int maxsockbuf = -1;
#else
    static long maxsockbuf = -1;
#endif
#endif

    /*
     * IPPROTO/IP_TOS :-
     * 1. IPv6 on Solaris/Mac OS:
     *    Set the TOS OR Traffic Class value to cater for
     *    IPv6 and IPv4 scenarios.
     * 2. IPv6 on Linux: By default Linux ignores flowinfo
     *    field so enable IPV6_FLOWINFO_SEND so that flowinfo
     *    will be examined. We also set the IPv4 TOS option in this case.
     * 3. IPv4: set socket option based on ToS and Precedence
     *    fields (otherwise get invalid argument)
     */
    if (level == IPPROTO_IP && opt == IP_TOS) {
        int *iptos;

#if defined(__linux__)
        if (ipv6_available()) {
            int optval = 1;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_FLOWINFO_SEND,
                           (void *)&optval, sizeof(optval)) < 0) {
                return -1;
            }
           /*
            * Let's also set the IPV6_TCLASS flag.
            * Linux appears to allow both IP_TOS and IPV6_TCLASS to be set
            * This helps in mixed environments where IPv4 and IPv6 sockets
            * are connecting.
            */
           if (setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS,
                           arg, len) < 0) {
                return -1;
            }
        }
#endif

        iptos = (int *)arg;
        *iptos &= (IPTOS_TOS_MASK | IPTOS_PREC_MASK);
    }

#ifdef _AIX
    if (level == SOL_SOCKET) {
        if (opt == SO_SNDBUF || opt == SO_RCVBUF) {
            /*
             * Just try to set the requested size. If it fails we will leave the
             * socket option as is. Setting the buffer size means only a hint in
             * the jse2/java software layer, see javadoc. In the previous
             * solution the buffer has always been truncated to a length of
             * 0x100000 Byte, even if the technical limit has not been reached.
             * This kind of absolute truncation was unexpected in the jck tests.
             */
            int ret = setsockopt(fd, level, opt, arg, len);
            if ((ret == 0) || (ret == -1 && errno == ENOBUFS)) {
                // Accept failure because of insufficient buffer memory resources.
                return 0;
            } else {
                // Deliver all other kinds of errors.
                return ret;
            }
        }
    }
#endif

    /*
     * On Linux the receive buffer is used for both socket
     * structures and the packet payload. The implication
     * is that if SO_RCVBUF is too small then small packets
     * must be discarded.
     */
#ifdef __linux__
    if (level == SOL_SOCKET && opt == SO_RCVBUF) {
        int *bufsize = (int *)arg;
        if (*bufsize < 1024) {
            *bufsize = 1024;
        }
    }
#endif

#if defined(_ALLBSD_SOURCE)
    /*
     * SOL_SOCKET/{SO_SNDBUF,SO_RCVBUF} - On FreeBSD need to
     * ensure that value is <= kern.ipc.maxsockbuf as otherwise we get
     * an ENOBUFS error.
     */
    if (level == SOL_SOCKET) {
        if (opt == SO_SNDBUF || opt == SO_RCVBUF) {
#ifdef KIPC_MAXSOCKBUF
            if (maxsockbuf == -1) {
               mib[0] = CTL_KERN;
               mib[1] = KERN_IPC;
               mib[2] = KIPC_MAXSOCKBUF;
               rlen = sizeof(maxsockbuf);
               if (sysctl(mib, 3, &maxsockbuf, &rlen, NULL, 0) == -1)
                   maxsockbuf = 1024;

#if 1
               /* XXXBSD: This is a hack to workaround mb_max/mb_max_adj
                  problem.  It should be removed when kern.ipc.maxsockbuf
                  will be real value. */
               maxsockbuf = (maxsockbuf/5)*4;
#endif
           }
#elif defined(__OpenBSD__)
           maxsockbuf = SB_MAX;
#else
           maxsockbuf = 64 * 1024;      /* XXX: NetBSD */
#endif

           bufsize = (int *)arg;
           if (*bufsize > maxsockbuf) {
               *bufsize = maxsockbuf;
           }

           if (opt == SO_RCVBUF && *bufsize < 1024) {
                *bufsize = 1024;
           }

        }
    }
#endif

#if defined(_ALLBSD_SOURCE) || defined(_AIX)
    /*
     * On Solaris, SO_REUSEADDR will allow multiple datagram
     * sockets to bind to the same port. The network jck tests check
     * for this "feature", so we need to emulate it by turning on
     * SO_REUSEPORT as well for that combination.
     */
    if (level == SOL_SOCKET && opt == SO_REUSEADDR) {
        int sotype;
        socklen_t arglen;

        arglen = sizeof(sotype);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (void *)&sotype, &arglen) < 0) {
            return -1;
        }

        if (sotype == SOCK_DGRAM) {
            if (setsockopt(fd, level, SO_REUSEPORT, arg, len) < 0) {
                return -1;
            }
        }
    }
#endif

    return setsockopt(fd, level, opt, arg, len);
}

/*
 * Wrapper for bind system call - performs any necessary pre/post
 * processing to deal with OS specific issues :-
 *
 * Linux allows a socket to bind to 127.0.0.255 which must be
 * caught.
 */
int
NET_Bind(int fd, SOCKETADDRESS *sa, int len)
{
    int rv;

#ifdef __linux__
    /*
     * ## get bugId for this issue - goes back to 1.2.2 port ##
     * ## When IPv6 is enabled this will be an IPv4-mapped
     * ## with family set to AF_INET6
     */
    if (sa->sa.sa_family == AF_INET) {
        if ((ntohl(sa->sa4.sin_addr.s_addr) & 0x7f0000ff) == 0x7f0000ff) {
            errno = EADDRNOTAVAIL;
            return -1;
        }
    }
#endif

    rv = bind(fd, &sa->sa, len);

    return rv;
}

/**
 * Wrapper for poll with timeout on a single file descriptor.
 *
 * flags (defined in net_util_md.h can be any combination of
 * NET_WAIT_READ, NET_WAIT_WRITE & NET_WAIT_CONNECT.
 *
 * The function will return when either the socket is ready for one
 * of the specified operations or the timeout expired.
 *
 * It returns the time left from the timeout (possibly 0), or -1 if it expired.
 */

jint
NET_Wait(JNIEnv *env, jint fd, jint flags, jint timeout)
{
    jlong prevNanoTime = JVM_NanoTime(env, 0);
    jlong nanoTimeout = (jlong) timeout * NET_NSEC_PER_MSEC;
    jint read_rv;

    while (1) {
        jlong newNanoTime;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = 0;
        if (flags & NET_WAIT_READ)
          pfd.events |= POLLIN;
        if (flags & NET_WAIT_WRITE)
          pfd.events |= POLLOUT;
        if (flags & NET_WAIT_CONNECT)
          pfd.events |= POLLOUT;

        errno = 0;
        read_rv = poll(&pfd, 1, nanoTimeout / NET_NSEC_PER_MSEC);

        newNanoTime = JVM_NanoTime(env, 0);
        nanoTimeout -= (newNanoTime - prevNanoTime);
        if (nanoTimeout < NET_NSEC_PER_MSEC) {
          return read_rv > 0 ? 0 : -1;
        }
        prevNanoTime = newNanoTime;

        if (read_rv > 0) {
          break;
        }
      } /* while */
    return (nanoTimeout / NET_NSEC_PER_MSEC);
}
