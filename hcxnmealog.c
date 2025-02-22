#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <libgen.h>
#include <linux/filter.h>
#include <linux/genetlink.h>
#include <linux/if_packet.h>
#include <linux/limits.h>
#include <linux/nl80211.h>
#include <linux/rtnetlink.h>
#include <linux/version.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/timerfd.h>
#include <termios.h>
#include <unistd.h>

#include "include/hcxnmealog.h"
#include "include/types.h"
#include "include/byteorder.h"
#include "include/ieee80211.h"
#include "include/radiotap.h"
/*===========================================================================*/
/* global variable */

static int fd_socket_rx = 0;
static int fd_gps = 0;
static int fd_timer = 0;
static int timerwaitnd = TIMER_EPWAITND;
static float latitude = 0;
static float longitude = 0;
static float altitude = 0;
static float lat = 0;
static float lon = 0;
static char ns = 0;
static char ew = 0;
static char rssi = 0;
static FILE *fh_nmea = NULL;
static ssize_t packetlen = 0;
static ssize_t nmealen = 0;
static u64 packetcount = 0;
static u64 nmeapacketcount = 0;
static u64 lifetime = 0;
static u32 errorcount = 0;
static u32 errorcountmax = ERROR_MAX;
static u16 ieee82011len = 0;
static u16 payloadlen = 0;
static u16 wanteventflag = 0;
static u8 *packetptr = NULL;
static u8 *ieee82011ptr = NULL;
static u8 *payloadptr = NULL;
static ieee80211_mac_t *macfrx = NULL;
static rth_t *rth = NULL;
static struct tpacket_stats lStats = { 0 };
static socklen_t lStatsLength = sizeof(lStats);
static struct sock_fprog bpf = { 0 };
static struct timespec tspecnmea = { 0 };
static struct timespec tspecakt = { 0 };
static char nmearxbuffer[NMEA_SIZE] = { 0 };
static char nmeaoutbuffer[NMEA_SIZE] = { 0 };
static u8 rx[PCAPNG_SNAPLEN * 2] = { 0 };
static u8 rxbuffer[PCAPNG_SNAPLEN * 2] = { 0 };
/*===========================================================================*/
static void close_devices()
{
if(fd_gps != 0) close(fd_gps);
if(fd_socket_rx != 0)
	{
	if(getsockopt(fd_socket_rx, SOL_PACKET, PACKET_STATISTICS, &lStats, &lStatsLength) != 0) fprintf(stdout, "PACKET_STATISTICS failed\n");
	close(fd_socket_rx);
	}
return;
}
/*===========================================================================*/
static size_t chop(char *buffer, size_t len)
{
char *ptr = NULL;

ptr = buffer +len - 1;
while(len)
	{
	if(*ptr != '\n') break;
	*ptr-- = 0;
	len--;
	}
while(len)
	{
	if(*ptr != '\r') break;
	*ptr-- = 0;
	len--;
	}
return len;
}
/*---------------------------------------------------------------------------*/
static int fgetline(FILE *inputstream, size_t size, char *buffer)
{
size_t len = 0;
char *buffptr = NULL;

if(feof(inputstream)) return -1;
buffptr = fgets(buffer, size, inputstream);
if(buffptr == NULL) return -1;
len = strlen(buffptr);
len = chop(buffptr, len);
return len;
}
/*===========================================================================*/
static bool read_bpf(char *bpfname)
{
static int len;
static struct sock_filter *bpfptr;
static FILE *fh_filter;
static char linein[128];

if((fh_filter = fopen(bpfname, "r")) == NULL) return false;
bpf.filter = (struct sock_filter*)calloc(BPF_MAXINSNS, sizeof(struct sock_filter));
bpf.len = 0;
bpfptr = bpf.filter;
while(bpf.len < BPF_MAXINSNS +1)
	{
	if((len = fgetline(fh_filter, 128, linein)) == -1) break;
	if(bpf.len == BPF_MAXINSNS)
		{
		bpf.len = 0;
		break;
		}
	if(len < 7) continue;
	if(linein[0] != '{')
		{
		if(sscanf(linein, "%" SCNu16 "%" SCNu8 "%" SCNu8 "%" SCNu32, &bpfptr->code, &bpfptr->jt, &bpfptr->jf, &bpfptr->k) != 4)
			{
			bpf.len = 0;
			break;
			}
		}
	else
		{
		if(sscanf(linein, "{ %" SCNx16 ", %"  SCNu8 ", %" SCNu8 ", %" SCNx32 " },",&bpfptr->code, &bpfptr->jt, &bpfptr->jf, &bpfptr->k) != 4)
			{
			bpf.len = 0;
			break;
			}
		}
	bpfptr++;
	bpf.len++;
	}
fclose(fh_filter);
if(bpf.len == 0) return false;
return true;
}
/*===========================================================================*/
static void close_files(void)
{
if(fh_nmea != NULL)fclose(fh_nmea);
return;
}
/*---------------------------------------------------------------------------*/
static bool open_files(char *nmeaoutname)
{
if(nmeaoutname == NULL) fh_nmea = stdout;
else if((fh_nmea = fopen(nmeaoutname, "a")) == NULL)
	{
	errorcount++;
	fprintf(stderr, "failed to open nmea file\n");
	return false;
	}
return true;
}
/*===========================================================================*/
static bool open_socket_rx(int ifaktindex, char *bpfname)
{
static size_t c = 10;
static struct sockaddr_ll saddr;
static struct packet_mreq mrq;
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0))
 static int enable = 1;
#endif
static int socket_rx_flags;
static int prioval;
static socklen_t priolen;

bpf.len = 0;
if(bpfname != NULL)
	{
	if(read_bpf(bpfname) == false)
		{
		errorcount++;
		fprintf(stderr, "failed to read BPF\n");
		return false;
		}
	}
if((fd_socket_rx = socket(PF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL))) < 0) return false;
memset(&mrq, 0, sizeof(mrq));
mrq.mr_ifindex = ifaktindex;
mrq.mr_type = PACKET_MR_PROMISC;
if(setsockopt(fd_socket_rx, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mrq, sizeof(mrq)) < 0) return false;
priolen = sizeof(prioval);
prioval = 20;
if(setsockopt(fd_socket_rx, SOL_SOCKET, SO_PRIORITY, &prioval, priolen) < 0) return false;
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0))
if(setsockopt(fd_socket_rx, SOL_PACKET, PACKET_IGNORE_OUTGOING, &enable, sizeof(int)) < 0) fprintf(stderr, "PACKET_IGNORE_OUTGOING is not supported by kernel\nfalling back to validate radiotap header length\n");
#endif
if(bpf.len > 0)
	{
	if(setsockopt(fd_socket_rx, SOL_SOCKET, SO_ATTACH_FILTER, &bpf, sizeof(bpf)) < 0)
		{
		fprintf(stderr, "failed to attach BPF (SO_ATTACH_FILTER): %s\n", strerror(errno));
		#ifdef HCXDEBUG
		fprintf(fh_debug, "SO_ATTACH_FILTER failed: %s\n", strerror(errno));
		#endif
		return false;
		}
	}
memset(&saddr, 0, sizeof(saddr));
saddr.sll_family = PF_PACKET;
saddr.sll_ifindex = ifaktindex;
saddr.sll_protocol = htons(ETH_P_ALL);
saddr.sll_halen = ETH_ALEN;
saddr.sll_pkttype = PACKET_OTHERHOST;
if(bind(fd_socket_rx, (struct sockaddr*) &saddr, sizeof(saddr)) < 0) return false;
if((socket_rx_flags = fcntl(fd_socket_rx, F_GETFL, 0)) < 0) return false;
if(fcntl(fd_socket_rx, F_SETFL, socket_rx_flags | O_NONBLOCK) < 0) return false;
while((!wanteventflag) || (c != 0))
	{
	packetlen = read(fd_socket_rx, rx, PCAPNG_SNAPLEN);
	if(packetlen == -1) break;
	c--;
	}
return true;
}
/*---------------------------------------------------------------------------*/
static bool open_socket_gpsd(void)
{
static int socket_gps_flags;
static struct sockaddr_in gpsd_addr;
static const char *gpsd_enable_nmea = "?WATCH={\"enable\":true,\"json\":false,\"nmea\":true}";

if((fd_gps = socket(AF_INET, SOCK_STREAM, 0)) < 0) return false;
memset(&gpsd_addr, 0, sizeof(struct sockaddr_in));
gpsd_addr.sin_family = AF_INET;
gpsd_addr.sin_port = htons(2947);
gpsd_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
if(connect(fd_gps, (struct sockaddr*) &gpsd_addr, sizeof(gpsd_addr)) < 0) return false;
if(fcntl(fd_gps, F_SETFL, socket_gps_flags | O_NONBLOCK) < 0) return false;
if(write(fd_gps, gpsd_enable_nmea, 47) != 47) return false;
return true;
}
/*---------------------------------------------------------------------------*/
static bool open_device_gps(char *gpsdevicename, int baudrate)
{
static struct termios tty;

if((fd_gps = open(gpsdevicename, O_RDONLY | O_NONBLOCK)) < 0) return false;
if(flock(fd_gps, LOCK_EX) < 0) return false;
if(tcgetattr(fd_gps, &tty) < 0) return false;
tty.c_cflag &= ~PARENB; // Clear parity bit, disabling parity (most common)
tty.c_cflag &= ~CSTOPB; // Clear stop field, only one stop bit used in communication (most common)
tty.c_cflag &= ~CSIZE; // Clear all bits that set the data size
tty.c_cflag |= CS8; // 8 bits per byte (most common)
tty.c_cflag &= ~CRTSCTS; // Disable RTS/CTS hardware flow control (most common)
tty.c_cflag |= CREAD | CLOCAL; // Turn on READ & ignore ctrl lines (CLOCAL = 1)
tty.c_lflag &= ~ICANON;
tty.c_lflag &= ~ECHO; // Disable echo
tty.c_lflag &= ~ECHOE; // Disable erasure
tty.c_lflag &= ~ECHONL; // Disable new-line echo
tty.c_lflag &= ~ISIG; // Disable interpretation of INTR, QUIT and SUSP
tty.c_iflag &= ~(IXON | IXOFF | IXANY); // Turn off s/w flow ctrl
tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL); // Disable any special handling of received bytes
tty.c_oflag &= ~OPOST; // Prevent special interpretation of output bytes (e.g. newline chars)
tty.c_oflag &= ~ONLCR; // Prevent conversion of newline to carriage return/line feed
tty.c_cc[VTIME] = 10;    // Wait for up to 1s (10 deciseconds), returning as soon as any data is received.
tty.c_cc[VMIN] = 0;
cfsetspeed(&tty, (speed_t)baudrate);
if (tcsetattr(fd_gps, TCSANOW, &tty) < 0) return false;
return true;
}
/*---------------------------------------------------------------------------*/
static bool open_devices(char *hcxnmealogname, int ifaktindex, char *bpfname, char *gpsdevice, int baudrate)
{
static char *gpsdname = "gpsd";
static char *devicename = "/dev";

if(ifaktindex != 0)
	{
	if(getuid() != 0)
		{
		errorcount++;
		fprintf(stderr, "%s must be run as root\n", hcxnmealogname);
		return false;
		}
	if(open_socket_rx(ifaktindex, bpfname) == false)
		{
		errorcount++;
		fprintf(stderr, "failed to open raw packet socket\n");
		return false;
		}
	}
if(strncmp(gpsdname, gpsdevice, 4) == 0)
	{
	if(open_socket_gpsd() == false)
		{
		fprintf(stderr, "failed to connect to GPSD\n");
		return EXIT_SUCCESS;
		}
	}
else if(strncmp(devicename, gpsdevice, 4) == 0)
	{
	if(open_device_gps(gpsdevice, baudrate) == false)
		{
		fprintf(stderr, "failed to open GPS device\n");
		return EXIT_SUCCESS;
		}
	}
else
	{
	fprintf(stderr, "no GPS device selected\n");
	return EXIT_SUCCESS;
	}



return true;
}
/*===========================================================================*/
/* SIGNALHANDLER */
static void signal_handler(int signum)
{
if((signum == SIGINT) || (signum == SIGTERM) || (signum == SIGKILL) || (signum == SIGTSTP)) wanteventflag |= EXIT_ON_SIGTERM;
return;
}
/*---------------------------------------------------------------------------*/
static bool set_signal_handler(void)
{
struct sigaction sa;

sa.sa_handler = signal_handler;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;
if(sigaction(SIGINT, &sa, NULL) < 0) return false;
if(sigaction(SIGTERM, &sa, NULL) < 0) return false;
if(sigaction(SIGTSTP, &sa, NULL) < 0) return false;
return true;
}
/*===========================================================================*/
/* TIMER */
static bool set_timer(void)
{
static struct itimerspec tval;

if((fd_timer = timerfd_create(CLOCK_BOOTTIME, 0)) < 0) return false;
tval.it_value.tv_sec = TIMER_VALUE_SEC;
tval.it_value.tv_nsec = TIMER_VALUE_NSEC;
tval.it_interval.tv_sec = TIMER_INTERVAL_SEC;
tval.it_interval.tv_nsec = TIMER_INTERVAL_NSEC;
if(timerfd_settime(fd_timer, 0, &tval, NULL) == -1) return false;
return true;
}
/*===========================================================================*/
static void global_deinit()
{
if(fd_timer != 0) close(fd_timer);
return;
}
/*---------------------------------------------------------------------------*/
static bool global_init(void)
{
packetptr = &rxbuffer[PCAPNG_SNAPLEN * 2];
if(set_signal_handler() == false)
	{
	errorcount++;
	fprintf(stderr, "failed to initialize signal handler\n");
	return false;
	}

if(set_timer() == false)
	{
	errorcount++;
	fprintf(stderr, "failed to initialize timer\n");
	return false;
	}

return true;
}
/*===========================================================================*/
static inline __attribute__((always_inline)) void process_nmea0183(void)
{
static int h;
static int m;
static int fix;
static int satcount;
static float s;
static float hdop;
static char v;
static char *nsen;
static char *nres;

nmearxbuffer[nmealen] = 0;
if((nmealen = read(fd_gps, nmearxbuffer, NMEA_SIZE)) < NMEA_MIN)
	{
	if(nmealen == - 1) errorcount++;
	return;
	}
clock_gettime(CLOCK_REALTIME, &tspecnmea);
nmeapacketcount++;
nmearxbuffer[nmealen] = 0;
nres = nmearxbuffer;
while((nsen = strsep(&nres, "\n\r")) != NULL)
	{
	if(strlen(nsen) < 6) continue;
	if(nsen[0] != '$') continue;
	fprintf(fh_nmea, "%s\n", nsen);
	if(nsen[3] == 'R')
		{
		if(nsen[4] == 'M')
			{
			if(nsen[5] == 'C')
				{
				latitude = 0;
				longitude = 0;
				ns = 0;
				ew = 0;
				sscanf(&nsen[7],"%02d%02d%f,%c,%f,%c,%f,%c", &h, &m, &s, &v, &lat, &ew, &lon, &ns);
				if(lat != 0) latitude = ((int)lat) /100 + (((int)lat) %100 +lat -(int)lat)/60;
				if(lon != 0) longitude = ((int)lon) /100 + (((int)lon) %100 +lon -(int)lon)/60;
				if(ew == 'W') latitude =-latitude;
				if(ns == 'S') longitude =-longitude;
				}
			}
		}
	else if(nsen[3] == 'G')
		{
		if(nsen[4] == 'G')
			{
			if(nsen[5] == 'A')
				{
				latitude = 0;
				longitude = 0;
				altitude = 0;
				ns = 0;
				ew = 0;
				sscanf(&nsen[7],"%02d%02d%f,%f,%c,%f,%c,%d,%d,%f,%f", &h, &m, &s, &lat, &ew, &lon, &ns, &fix, &satcount, &hdop, &altitude);
				if(lat != 0) latitude = ((int)lat) /100 + (((int)lat) %100 +lat -(int)lat)/60;
				if(lon != 0) longitude = ((int)lon) /100 + (((int)lon) %100 +lon -(int)lon)/60;
				if(ew == 'W') latitude =-latitude;
				if(ns == 'S') longitude =-longitude;
				}
			}
		}
	}
fflush(fh_nmea);
return;
}
/*===========================================================================*/
static u8 getradiotapfield(uint16_t rthlen)
{
static int i;
static uint16_t pf;
static rth_t *rth;
static uint32_t *pp;

rth = (rth_t*)packetptr;
pf = RTHRX_SIZE;
if((rth->it_present & IEEE80211_RADIOTAP_EXT) == IEEE80211_RADIOTAP_EXT)
	{
	pp = (uint32_t*)packetptr;
	for(i = 2; i < rthlen /4; i++)
		{
		#ifdef BIG_ENDIAN_HOST
		pp[i] = byte_swap_32(pp[i]);
		#endif
		pf += 4;
		if((pp[i] & IEEE80211_RADIOTAP_EXT) != IEEE80211_RADIOTAP_EXT) break;
		}
	}
if((rth->it_present & IEEE80211_RADIOTAP_TSFT) == IEEE80211_RADIOTAP_TSFT)
	{
	if(pf > rthlen) return 0;
	if((pf %8) != 0) pf += 4;
	pf += 8;
	}
if((rth->it_present & IEEE80211_RADIOTAP_FLAGS) == IEEE80211_RADIOTAP_FLAGS)
	{
	if(pf > rthlen) return 0;
	pf += 1;
	}
if((rth->it_present & IEEE80211_RADIOTAP_RATE) == IEEE80211_RADIOTAP_RATE) pf += 1;
if((rth->it_present & IEEE80211_RADIOTAP_CHANNEL) == IEEE80211_RADIOTAP_CHANNEL)
	{
	if(pf > rthlen) return 0;
	if((pf %2) != 0) pf += 1;
	pf += 4;
	}
if((rth->it_present & IEEE80211_RADIOTAP_FHSS) == IEEE80211_RADIOTAP_FHSS)
		{
		if((pf %2) != 0) pf += 1;
		pf += 2;
		}
if((rth->it_present & IEEE80211_RADIOTAP_DBM_ANTSIGNAL) == IEEE80211_RADIOTAP_DBM_ANTSIGNAL) return packetptr[pf];
return 0;
}
/*---------------------------------------------------------------------------*/
static inline __attribute__((always_inline)) void process80211proberesponse(void)
{
clock_gettime(CLOCK_REALTIME, &tspecakt);
rssi = getradiotapfield(__hcx16le(rth->it_len));
if(tspecakt.tv_sec != tspecnmea.tv_sec) return; 
if(rssi == 0) return;
if(lon == 0) return;
if(lat == 0) return;


return;
}
/*---------------------------------------------------------------------------*/
static inline __attribute__((always_inline)) void process80211beacon(void)
{
static int cs;
static size_t nl;
static size_t cp;

clock_gettime(CLOCK_REALTIME, &tspecakt);
rssi = getradiotapfield(__hcx16le(rth->it_len));
if(tspecakt.tv_sec != tspecnmea.tv_sec) return; 
if(rssi == 0) return;
if(lon == 0) return;
if(lat == 0) return;


if(fh_nmea != NULL)
	{
	snprintf(nmeaoutbuffer, NMEA_SIZE, "$GPWPL,%10.5f,%c,%011.5f,%c,%02X%02X%02X%02X%02X%02X",lat, ew, lon, ns, macfrx->addr3[0], macfrx->addr3[1], macfrx->addr3[2], macfrx->addr3[3], macfrx->addr3[4], macfrx->addr3[5]);
	nl = strnlen(nmeaoutbuffer, NMEA_SIZE);
	cs = 0;
	for(cp = 1; cp < nl; cp++) cs = cs ^ nmeaoutbuffer[cp];
	fprintf(fh_nmea, "%s*%02X\r\n", nmeaoutbuffer, cs);
	}
return;
}
/*---------------------------------------------------------------------------*/
static inline __attribute__((always_inline)) void process_packet(void)
{
if((packetlen = read(fd_socket_rx, packetptr, PCAPNG_SNAPLEN)) < RTHRX_SIZE)
	{
	if(packetlen == -1) errorcount++;
	return;
	}
rth = (rth_t*)packetptr;
if((__hcx32le(rth->it_present) & IEEE80211_RADIOTAP_DBM_ANTSIGNAL) == 0) return;
if(__hcx16le(rth->it_len) > packetlen)
	{
	errorcount++;
	return;
	}
ieee82011ptr = packetptr + __hcx16le(rth->it_len);
ieee82011len = packetlen - __hcx16le(rth->it_len);
if(ieee82011len <= MAC_SIZE_RTS) return;
macfrx = (ieee80211_mac_t*)ieee82011ptr;
if((macfrx->from_ds == 1) && (macfrx->to_ds == 1))
	{
	payloadptr = ieee82011ptr +MAC_SIZE_LONG;
	payloadlen = ieee82011len -MAC_SIZE_LONG;
	}
else
	{
	payloadptr = ieee82011ptr +MAC_SIZE_NORM;
	payloadlen = ieee82011len -MAC_SIZE_NORM;
	}
packetcount++;
if(macfrx->type == IEEE80211_FTYPE_MGMT)
	{
	if(macfrx->subtype == IEEE80211_STYPE_BEACON) process80211beacon();
	else if(macfrx->subtype == IEEE80211_STYPE_PROBE_RESP) process80211proberesponse();
	}
return;
}
/*===========================================================================*/
/* GPS LOOPs */
static bool gps_loop(char *basename, char *nmeaoutname)
{
static ssize_t i;
static int fd_epoll = 0;
static int epi = 0;
static int epret = 0;
static u64 timercount;
static struct epoll_event ev, events[EPOLL_EVENTS_MAX];

if((fd_epoll= epoll_create(1)) < 0) return false;
ev.data.fd = fd_gps;
ev.events = EPOLLIN;
if(epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_gps, &ev) < 0) return false;
epi++;

ev.data.fd = fd_socket_rx;
ev.events = EPOLLIN;
if(epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_socket_rx, &ev) < 0) return false;
epi++;

ev.data.fd = fd_timer;
ev.events = EPOLLIN;
if(epoll_ctl(fd_epoll, EPOLL_CTL_ADD, fd_timer, &ev) < 0) return false;
epi++;

fprintf(stdout, "\033[?25l");
if(nmeaoutname != NULL)
	{
	fprintf(stdout, "%s %s logging NMEA 0183 track to %s\n", basename, VERSION_TAG, nmeaoutname);
	fprintf(stdout, "\rNMEA 0183 sentences: %" PRIu64 " (lat:%.1f lon:%.1f alt:%.1f) | 802.11 packets: %" PRIu64, nmeapacketcount, latitude, longitude, altitude, packetcount);
	}
while(!wanteventflag)
	{
	if(errorcount > errorcountmax) wanteventflag |= EXIT_ON_ERROR;
	epret = epoll_pwait(fd_epoll, events, epi, timerwaitnd, NULL);
	if(epret == -1)
		{
		if(errno != EINTR)
			{
			errorcount++;
			}
		continue;
		}
	for(i = 0; i < epret; i++)
		{
		if(events[i].data.fd == fd_gps) process_nmea0183();
		else if(events[i].data.fd == fd_socket_rx) process_packet();
		else if(events[i].data.fd == fd_timer)
			{
			if(read(fd_timer, &timercount, sizeof(u64)) == -1) errorcount++;
			lifetime++;
			if((lifetime %10) == 0)
				{
				if(nmeaoutname != NULL)
					{
					fprintf(stdout, "\rNMEA 0183 sentences: %" PRIu64 " (lat:%f lon:%f alt:%.1f) | 802.11 packets: %" PRIu64, nmeapacketcount, latitude, longitude, altitude, packetcount);
					}
				}
			}
		}
	}
fprintf(stdout, "\n\033[?25h");
return true;
}
/*===========================================================================*/
__attribute__ ((noreturn))
static void version(char *eigenname)
{
fprintf(stdout, "%s %s (C) %s ZeroBeat\n", eigenname, VERSION_TAG, VERSION_YEAR);
exit(EXIT_SUCCESS);
}
/*---------------------------------------------------------------------------*/
__attribute__ ((noreturn))
static void usage(char *eigenname)
{
fprintf(stdout, "%s %s (C) %s ZeroBeat\n"
	"usage:\n"
	"%s <options>\n"
	"\n"
	"options:\n"
	"-o <file>      : output nmea 0183 track\n"
	"                  track append to file: filename\n"
	"                  use gpsbabel to convert to other formats:\n"
	"                   gpsbabel -w -t -i nmea -f in_file.nmea -o gpx -F out_file.gpx\n"
	"                   gpsbabel -w -t -i nmea -f in_file.nmea -o kml -F out_file.kml\n"
	"-d <device>    : GPS source\n"
	"                  use gpsd: gpsd\n"
	"                  use device: /dev/ttyACM0, /dev/tty/USBx, ...\n"
	"                  get more information: https://en.wikipedia.org/wiki/NMEA_0183\n"
	"-b <digit>     : baudrate of GPS device\n"
	"                  default: 9600\n"
	"-i <INTERFACE> : name of INTERFACE to be used\n"
	"-h             : show this help\n"
	"-v             : show version\n"
	"\n"
	"--bpf=<file>   : input Berkeley Packet Filter (BPF) code (maximum %d instructions) in tcpdump decimal numbers format\n"
	"                  see --help for more information\n"
	"--help         : show this help\n"
	"--version      : show version\n"
	"\n", eigenname, VERSION_TAG, VERSION_YEAR, eigenname, BPF_MAXINSNS);
exit(EXIT_SUCCESS);
}
/*---------------------------------------------------------------------------*/
__attribute__ ((noreturn))
static void usageerror(char *eigenname)
{
fprintf(stdout, "%s %s (C) %s by ZeroBeat\n"
	"usage: %s -h for help\n", eigenname, VERSION_TAG, VERSION_YEAR, eigenname);
exit(EXIT_FAILURE);
}
/*===========================================================================*/
int main(int argc, char *argv[])
{
static int auswahl;
static int index;
static int ifaktindex;
static int baudrate;
static char *gpsdevice;
static char *nmeaoutname;
static char *bpfname;

static const char *short_options = "o:d:b:i:hv";
static const struct option long_options[] =
{
	{"bpf",				required_argument,	NULL,	HCX_BPF},
	{"version",			no_argument,		NULL,	HCX_VERSION},
	{"help",			no_argument,		NULL,	HCX_HELP},
	{NULL,				0,			NULL,	0}
};

auswahl = -1;
index = 0;
optind = 1;
optopt = 0;
baudrate = 9600;
ifaktindex = 0;
gpsdevice = NULL;
nmeaoutname = NULL;
bpfname = NULL;

while((auswahl = getopt_long (argc, argv, short_options, long_options, &index)) != -1)
	{
	switch (auswahl)
		{
		case HCX_IFNAME:
		if((ifaktindex = if_nametoindex(optarg)) == 0)
			{
			perror("failed to get interface index");
			exit(EXIT_FAILURE);
			}
		break;

		case HCX_BPF:
		bpfname = optarg;
		break;

		case HCX_GPS_DEVICE:
		gpsdevice = optarg;
		break;

		case HCX_GPS_BAUDRATE:
		baudrate = atoi(optarg);
		break;

		case HCX_OUTPUT_NMEA:
		nmeaoutname = optarg;
		break;

		case HCX_HELP:
		usage(basename(argv[0]));
		break;

		case HCX_VERSION:
		version(basename(argv[0]));
		break;

		case '?':
		usageerror(basename(argv[0]));
		break;
		}
	}
if(argc < 2)
	{
	fprintf(stderr, "no option selected\n");
	return EXIT_SUCCESS;
	}
setbuf(stdout, NULL);

if(open_devices(basename(argv[0]), ifaktindex, bpfname, gpsdevice, baudrate) == false) goto byebye;
if(open_files(nmeaoutname) == false) goto byebye;
if(global_init() == false) goto byebye;

if(gps_loop(basename(argv[0]), nmeaoutname) == false)
	{
	errorcount++;
	fprintf(stderr, "failed to initialize main scan loop\n");
	}

byebye:
close_devices();
close_files();
global_deinit();
if(nmeaoutname != NULL)
	{
	fprintf(stdout, "\nSummary:\n"
			"-------\n"
			"NMEA 0183 sentences logged.......: %" PRIu64 
			"802.11 packets received by kernel: %d"
			"802.11 packets dropped by kernel.: %d"
			"\n", nmeapacketcount, lStats.tp_packets, lStats.tp_drops);
	}
return EXIT_SUCCESS;
}
/*===========================================================================*/
