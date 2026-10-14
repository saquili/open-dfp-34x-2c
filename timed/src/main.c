// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * timed: sets the clock of the stick from the host, by NTP and by PTP.
 *
 *   timed [-n SERVER] [-p IFACE] [-o FILE] [-t FILE] [-M] [-d DOMAIN] [-s FILE] [-f] [-v] [-D]
 *   timed -H IFACE
 *
 * -n names the NTP server, an address (the stick runs offline and resolves
 * no name); -p the interface of the management link, on which PTP is listened
 * for over layer 2 and over UDP; -o the file in which omcid writes the time
 * the OLT gave (olt.h), looked at once a second; -t the file in which omcid
 * tells the time of day the OLT gave the PHC (pon.h), looked at once a second;
 * -M lets timed be the PTP master on the interface of -p while it hears no
 * better clock there and its clock holds a time worth giving; -d the PTP domain, 0 unless
 * given; -s the file that tells the state (/run/timed/state); -f keeps timed
 * in the foreground, its log on stderr; -v logs each sample; -D sets nothing
 * (a dry run, for the tests and for looking at what the host gives). -H turns
 * on the hardware stamps of an interface and exits; it is run as root, before
 * the daemon, which may not.
 *
 * Only one source sets the clock at a time (select.h). It is stepped when it
 * is more than 128 ms out, and steered by its frequency otherwise, by the
 * servo of servo.h; the OLT's time, which is to the second, only steps it when
 * it is a second out or more. The stamps are the kernel's, taken when a frame
 * is received, and the clock's when one is sent.
 *
 * Where the interface stamps PTP over layer 2 in hardware, those stamps are
 * taken instead, in the clock of the interface (its PHC): the PHC is set from
 * the master as the system clock would be, and the system clock follows the
 * PHC, read against it once a second (phc.h), as ptp4l and phc2sys of
 * linuxptp do between them. While no master does, the OLT's seconds, which
 * the PHC reports when the stick is on a PON (pon.h), steer its rate; on the
 * OLT's time of day the PHC holds the OLT's time, and the system clock follows
 * it. As a master (-M) timed gives the PHC's time, which then holds the OLT's
 * time or follows the system clock.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ntp.h"
#include "olt.h"
#include "phc.h"
#include "pon.h"
#include "ptp.h"
#include "select.h"
#include "servo.h"
#include "stamp.h"

#ifndef ETH_P_1588
#define ETH_P_1588 0x88F7
#endif
#ifndef ADJ_NANO
#define ADJ_NANO 0x2000
#endif
#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET 0x0100
#endif
#ifndef STA_NANO
#define STA_NANO 0x2000
#endif
#ifndef SO_TIMESTAMPNS
#define SO_TIMESTAMPNS 35
#endif
#ifndef SCM_TIMESTAMPNS
#define SCM_TIMESTAMPNS SO_TIMESTAMPNS
#endif

#ifndef SOF_TIMESTAMPING_OPT_TSONLY
#define SOF_TIMESTAMPING_OPT_TSONLY (1 << 11)
#endif
#ifndef PTP_EXTTS_REQUEST2
#define PTP_EXTTS_REQUEST2 PTP_EXTTS_REQUEST
#endif

/* The clock of a PTP device, open as @fd (clock_gettime(2)). */
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | 3)

#define DEFAULT_STATE "/run/timed/state"

/* A clock out by more than this is stepped; less is slewed (as ntpd: 128 ms). */
#define STEP_LIMIT (128 * NS_PER_MS)

/* The PTP addresses (IEEE 1588-2008 annex D and F). */
#define PTP_EVENT_PORT 319
#define PTP_GENERAL_PORT 320
#define PTP_UDP_GROUP "224.0.1.129"
static const uint8_t ptp_l2_group[6] = { 0x01, 0x1b, 0x19, 0x00, 0x00, 0x00 };

/* The NTP poll intervals: quick at first, then 16 s, then 64 s once the filter is full. */
#define NTP_PORT "123"
#define NTP_POLL_FIRST (2 * NS_PER_SEC)
#define NTP_POLL_NORMAL (16 * NS_PER_SEC)
#define NTP_POLL_STEADY (64 * NS_PER_SEC)
#define NTP_REPLY_WAIT (3 * NS_PER_SEC)

/* A Delay_Req each so many seconds: often until the delay is known. */
#define PTP_DELAY_REQ_FIRST (2 * NS_PER_SEC)
#define PTP_DELAY_REQ_NORMAL (16 * NS_PER_SEC)

/*
 * The system clock is read against the PHC once a second. A master heard over
 * layer 2 with hardware stamps within the last 4 s is that way only: its
 * messages over UDP, stamped in software, are not mixed in.
 */
#define PHC_FOLLOW_PERIOD NS_PER_SEC
#define PHC_L2_HOLD (4 * NS_PER_SEC)

#define REOPEN_PERIOD (5 * NS_PER_SEC)
#define STATE_PERIOD (10 * NS_PER_SEC)

/*
 * The OLT's file is looked at once a second; the time it gave holds for ten
 * minutes, and after that the clock runs on by itself (holdover), as the OLT
 * gave one reading and no more.
 */
#define OLT_PERIOD NS_PER_SEC
#define OLT_HOLD (10 * 60 * NS_PER_SEC)

/*
 * The PHC is asked for the OLT's seconds once a minute until it gives them:
 * the GPON MAC's driver may come after timed. They count as coming while the
 * last is at most 3 s old.
 */
#define PON_RETRY (60 * NS_PER_SEC)
#define PON_HOLD (3 * NS_PER_SEC)

/*
 * The time of day the OLT gave is taken as in the PHC once the PHC jumps under
 * the OLT's seconds after omcid told it, or, when it does not, 30 s after:
 * the PHC held a time within a millisecond of it already.
 */
#define PON_TOD_WAIT (30 * NS_PER_SEC)

/* What steers the PHC: nothing, a PTP master, the OLT's seconds, or the system clock. */
enum phc_owner { PHC_FREE, PHC_PTP, PHC_PON, PHC_SYS };

/* As a master: an Announce and a Sync once a second. */
#define MASTER_PERIOD NS_PER_SEC

/* A rate of the PHC's no crystal runs at: one an owner left is measured again. */
#define PHC_SANE_PPB 300000.0

enum transport { VIA_NONE, VIA_L2, VIA_UDP };

struct daemon {
	/* options */
	const char *ntp_server;
	const char *iface;
	const char *olt_path;
	const char *tod_path;
	bool master_offer;		/* -M: may be the PTP master */
	uint8_t domain;
	const char *state_path;
	bool verbose;
	bool foreground;
	bool dry;
	bool dirty;			/* the state file is out of date */

	/* the choice of source and the clock */
	struct select sel;
	struct servo servo;
	bool clock_set;			/* the clock has been set by a source since start */
	unsigned int steps;
	enum source last_src;
	ns_t last_offset;
	ns_t last_applied_at;

	/* NTP */
	int ntp_fd;
	struct sockaddr_storage ntp_addr;
	socklen_t ntp_addrlen;
	ns_t ntp_next;			/* monotonic time of the next query */
	ns_t ntp_t1;			/* local time the query went out */
	ns_t ntp_sent_mono;
	bool ntp_waiting;
	unsigned int ntp_polls;
	unsigned int ntp_misses;
	struct ntp_filter ntp_filter;
	struct ntp_sample ntp_last;
	bool ntp_have_last;

	/* PTP */
	int l2_fd;
	int event_fd;
	int general_fd;
	int ifindex;
	struct ptp_slave ptp;
	bool ptp_ready;
	enum transport via;		/* the way the master was heard */
	ns_t ptp_reopen_at;
	ns_t delay_req_at;
	ns_t ptp_last_offset;
	bool ptp_have_offset;
	struct median3 ptp_med;	/* the PTP offsets, filtered */

	/*
	 * PTP with hardware stamps: the PHC of the interface, set from the
	 * master by a servo of its own, and the system clock read against it.
	 */
	bool hw_ready;			/* the layer-2 socket takes hardware stamps */
	int phc_fd;
	int phc_index;
	clockid_t phc;
	struct servo phc_servo;
	struct median3 phc_med;		/* the PHC's offsets from the master, filtered */
	bool phc_set;			/* the PHC holds the master's time */
	unsigned int phc_steps;
	ns_t l2_sync_at;		/* monotonic time of the last Sync stamped in hardware */
	ns_t phc_follow_at;
	ns_t sys_phc_offset;		/* the system clock less the PHC's time in UTC */
	ns_t sys_phc_window;		/* the time the kernel took to read the PHC */
	bool have_sys_phc;
	unsigned int tx_stamps;		/* Delay_Req stamped in hardware */
	unsigned int tx_missed;		/* and those whose stamp never came */
	enum phc_owner phc_owner;

	/*
	 * The OLT's seconds, external timestamps of the PHC (pon.h), which
	 * steer it while no master does.
	 */
	bool pon_ready;			/* the PHC gives them */
	bool pon_none_said;		/* the log said it gives none */
	ns_t pon_retry_at;
	struct pon pon;
	ns_t pon_at;			/* monotonic time of the last second */
	ns_t pon_offset;		/* the PHC's offset from them, filtered, last */
	bool pon_have_offset;
	bool pon_in_step;		/* the log said the PHC follows them */
	unsigned int pon_seconds;	/* seconds that gave an offset */
	unsigned int pon_jumps;		/* times the PHC jumped under them */

	/* The time of day the OLT gave the PHC (pon.h), as omcid told it. */
	ns_t tod_check_at;
	struct timespec tod_mtime;
	off_t tod_size;
	bool tod_have;
	struct pon_tod tod;
	bool pon_tod_set;		/* the PHC holds the OLT's time */
	ns_t pon_follow_at;

	/* The PHC on the system clock, for a master whose time is the system clock's. */
	ns_t phc_sys_at;
	bool phc_sys_in_step;

	/* The PTP master (-M). */
	bool hw_onestep;		/* the interface writes the time into a Sync as it leaves */
	struct ptp_master master;
	bool is_master;
	ns_t master_next_at;
	unsigned int syncs_sent;
	unsigned int delay_resps_sent;

	/* the OLT's time: the file as last read, and what it said */
	ns_t olt_check_at;
	struct timespec olt_mtime;
	off_t olt_size;
	bool olt_have;
	ns_t olt_utc;
	ns_t olt_at;
};

static bool ptp_master_better(const struct daemon *d, ns_t mono);

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* ---- log ---- */

static void say(const struct daemon *d, int prio, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (d->foreground) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	} else {
		vsyslog(prio, fmt, ap);
	}
	va_end(ap);
}

#define LOG(d, ...) say(d, LOG_NOTICE, __VA_ARGS__)
#define WARN(d, ...) say(d, LOG_WARNING, __VA_ARGS__)
#define DEBUG(d, ...) do { if ((d)->verbose) say(d, LOG_INFO, __VA_ARGS__); } while (0)

/* ---- the clocks ---- */

static ns_t clock_ns(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return (ns_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static ns_t real_now(void)
{
	return clock_ns(CLOCK_REALTIME);
}

static ns_t mono_now(void)
{
	return clock_ns(CLOCK_MONOTONIC);
}

/* A signed time as "+1.234567 s". */
static const char *show(ns_t t, char *buf, size_t cap)
{
	ns_t a = t < 0 ? -t : t;

	snprintf(buf, cap, "%c%lld.%06lld s", t < 0 ? '-' : '+', (long long)(a / NS_PER_SEC),
		 (long long)(a % NS_PER_SEC / 1000));
	return buf;
}

/*
 * The kernel's frequency is in parts per million scaled by 2^16; the servo's
 * is in parts per billion, positive to slow the clock down.
 */
#define PPB_TO_FREQ (-65.536)

/*
 * Set the clock's frequency, with the kernel's own loop off and the clock
 * told synchronised (the status word cleared but for its read-only bits).
 */
static int freq_set(const struct daemon *d, double ppb)
{
	struct timex tx;

	if (d->dry)
		return 0;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY | ADJ_STATUS | ADJ_MAXERROR | ADJ_ESTERROR;
	tx.freq = (long)(ppb * PPB_TO_FREQ);
	tx.status = 0;
	tx.maxerror = 1000;
	tx.esterror = 1000;
	return adjtimex(&tx) < 0 ? -1 : 0;
}

/*
 * At the start: end what the kernel's loop may still be slewing (an offset of
 * a daemon before this one), and take the frequency the clock runs at now,
 * which a timed started again by procd left right.
 */
static double freq_start(const struct daemon *d)
{
	struct timex tx;
	double freq;

	if (d->dry)
		return 0;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_OFFSET | ADJ_STATUS | ADJ_NANO;
	tx.status = STA_PLL;
	tx.offset = 0;
	if (adjtimex(&tx) < 0)
		return 0;
	freq = (double)tx.freq / PPB_TO_FREQ;
	/* The kernel's loop off, and the clock not synchronised yet. */
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_STATUS;
	tx.status = STA_UNSYNC;
	adjtimex(&tx);
	return freq;
}

static int clock_step(const struct daemon *d, ns_t offset)
{
	ns_t t = real_now() - offset;
	struct timespec ts;

	if (d->dry)
		return 0;
	ts.tv_sec = (time_t)(t / NS_PER_SEC);
	ts.tv_nsec = (long)(t % NS_PER_SEC);
	if (ts.tv_nsec < 0) {
		ts.tv_nsec += NS_PER_SEC;
		ts.tv_sec -= 1;
	}
	if (clock_settime(CLOCK_REALTIME, &ts) < 0)
		return -1;
	freq_set(d, d->servo.freq);
	return 0;
}

/* ---- the state file ---- */

/*
 * locked: a source in use, and the clock within a millisecond of it; syncing:
 * a source in use, and the clock still farther; set: the OLT's time in use,
 * which is to the second; holdover: no source now, but the clock was set and
 * runs on; unsynchronized: never set since the start.
 */
static const char *state_name(const struct daemon *d)
{
	if (d->sel.cur == SRC_OLT)
		return "set";
	if (d->sel.cur != SRC_NONE) {
		ns_t off = d->last_offset < 0 ? -d->last_offset : d->last_offset;

		return off < NS_PER_MS ? "locked" : "syncing";
	}
	return d->clock_set ? "holdover" : "unsynchronized";
}

static void write_state(const struct daemon *d, ns_t mono)
{
	char tmp[256];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", d->state_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "source=%s\n", source_name(d->sel.cur));
	fprintf(f, "state=%s\n", state_name(d));
	fprintf(f, "offset_ns=%lld\n", (long long)d->last_offset);
	fprintf(f, "updated=%lld\n", (long long)(real_now() / NS_PER_SEC));
	fprintf(f, "steps=%u\n", d->steps);
	fprintf(f, "freq_ppb=%lld\n", (long long)-d->servo.freq);
	if (d->ntp_server) {
		fprintf(f, "ntp_server=%s\n", d->ntp_server);
		fprintf(f, "ntp_good=%d\n", select_good(&d->sel, SRC_NTP, mono));
		if (d->ntp_have_last) {
			fprintf(f, "ntp_offset_ns=%lld\n", (long long)d->sel.s[SRC_NTP].offset);
			fprintf(f, "ntp_delay_ns=%lld\n", (long long)d->ntp_last.delay);
			fprintf(f, "ntp_stratum=%u\n", d->ntp_last.stratum);
		}
	}
	if (d->iface) {
		fprintf(f, "ptp_interface=%s\n", d->iface);
		fprintf(f, "ptp_good=%d\n", select_good(&d->sel, SRC_PTP, mono));
		if (d->ptp.have_master) {
			const uint8_t *c = d->ptp.master.clock;

			fprintf(f, "ptp_master=%02x%02x%02x%02x%02x%02x%02x%02x\n", c[0], c[1], c[2], c[3],
				c[4], c[5], c[6], c[7]);
			fprintf(f, "ptp_via=%s\n", d->via == VIA_L2 ? "l2" : "udp");
			fprintf(f, "ptp_delay_ns=%lld\n", (long long)d->ptp.delay);
		}
		if (d->ptp_have_offset)
			fprintf(f, "ptp_offset_ns=%lld\n", (long long)d->ptp_last_offset);
		fprintf(f, "ptp_stamps=%s\n", d->hw_ready ? "hardware" : "software");
		if (d->hw_ready) {
			fprintf(f, "ptp_phc=/dev/ptp%d\n", d->phc_index);
			fprintf(f, "ptp_phc_set=%d\n", d->phc_set);
			fprintf(f, "ptp_phc_freq_ppb=%lld\n", (long long)-d->phc_servo.freq);
			fprintf(f, "ptp_phc_steps=%u\n", d->phc_steps);
			if (d->have_sys_phc) {
				fprintf(f, "ptp_sys_phc_offset_ns=%lld\n", (long long)d->sys_phc_offset);
				fprintf(f, "ptp_sys_phc_window_ns=%lld\n", (long long)d->sys_phc_window);
			}
			fprintf(f, "ptp_tx_stamps=%u\n", d->tx_stamps);
			fprintf(f, "ptp_tx_missed=%u\n", d->tx_missed);
		}
	}
	if (d->pon_ready) {
		bool coming = mono - d->pon_at < PON_HOLD;

		fprintf(f, "pon_seconds=%d\n", coming);
		fprintf(f, "pon_steers=%d\n", coming && d->phc_owner == PHC_PON);
		if (d->pon_have_offset)
			fprintf(f, "pon_offset_ns=%lld\n", (long long)d->pon_offset);
		fprintf(f, "pon_count=%u\n", d->pon_seconds);
		fprintf(f, "pon_jumps=%u\n", d->pon_jumps);
	}
	if (d->tod_path) {
		fprintf(f, "pon_tod_file=%s\n", d->tod_path);
		fprintf(f, "pon_tod_set=%d\n", d->pon_tod_set);
		fprintf(f, "pon_good=%d\n", select_good(&d->sel, SRC_PON, mono));
		if (d->tod_have) {
			fprintf(f, "pon_tod_frame=%u\n", d->tod.superframe);
			fprintf(f, "pon_tod_sec=%lld\n", (long long)(d->tod.tstamp / NS_PER_SEC));
		}
	}
	if (d->hw_ready) {
		static const char *const owner[] = { "none", "ptp", "pon", "system" };

		fprintf(f, "ptp_phc_owner=%s\n", owner[d->phc_owner]);
	}
	if (d->master_offer) {
		fprintf(f, "ptp_role=%s\n", d->is_master ? "master" :
			ptp_master_better(d, mono) ? "slave" : "listening");
		fprintf(f, "ptp_syncs_sent=%u\n", d->syncs_sent);
		fprintf(f, "ptp_delay_resps_sent=%u\n", d->delay_resps_sent);
	}
	if (d->olt_path) {
		fprintf(f, "olt_file=%s\n", d->olt_path);
		fprintf(f, "olt_good=%d\n", select_good(&d->sel, SRC_OLT, mono));
		if (d->olt_have) {
			fprintf(f, "olt_utc=%lld\n", (long long)(d->olt_utc / NS_PER_SEC));
			fprintf(f, "olt_offset_ns=%lld\n", (long long)d->sel.s[SRC_OLT].offset);
		}
	}
	if (fclose(f) == 0)
		rename(tmp, d->state_path);
}

/* ---- applying a sample ---- */

/* Step the clock by @offset; the stamps taken before are of a clock that is no more. */
static bool step(struct daemon *d, enum source src, ns_t offset)
{
	char buf[32];

	if (clock_step(d, offset) < 0) {
		WARN(d, "cannot set the clock: %s", strerror(errno));
		return false;
	}
	d->clock_set = true;
	d->steps++;
	d->dirty = true;
	ntp_filter_init(&d->ntp_filter);
	/* Stamps in the PHC are not of the system clock: a step leaves them. */
	if (!d->ptp.local_timescale) {
		ptp_clock_stepped(&d->ptp);
		median3_reset(&d->ptp_med);
	}
	LOG(d, "clock stepped by %s from %s", show(-offset, buf, sizeof(buf)), source_name(src));
	return true;
}

static void discipline(struct daemon *d, enum source src, ns_t offset, ns_t at)
{
	char buf[32];
	double freq = 0;

	d->last_src = src;
	d->last_offset = offset;
	/* The OLT's time is to the second: within that it says nothing, and it never steers. */
	if (src == SRC_OLT) {
		if (offset >= OLT_RESOLUTION || offset <= -OLT_RESOLUTION) {
			if (step(d, src, offset))
				servo_reset(&d->servo);
		} else if (!d->clock_set) {
			d->clock_set = true;
			d->dirty = true;
			LOG(d, "clock agrees with olt to the second");
		}
		return;
	}
	d->servo.baseline = src == SRC_NTP ? SERVO_BASELINE_NTP : SERVO_BASELINE_PTP;
	/* PTP with hardware stamps, and the PON, give the PHC, which the system clock reads near exactly. */
	d->servo.gains = (src == SRC_PTP && d->ptp.local_timescale) || src == SRC_PON ? SERVO_GAINS_HARD
										     : SERVO_GAINS_SOFT;
	switch (servo_sample(&d->servo, offset, at, &freq)) {
	case SERVO_STEP:
		step(d, src, offset);
		return;
	case SERVO_JUMP:
		if (step(d, src, offset) && freq_set(d, freq) < 0)
			WARN(d, "cannot set the clock's frequency: %s", strerror(errno));
		DEBUG(d, "%s: rate %+.0f ppb", source_name(src), -freq);
		return;
	case SERVO_FREQ:
		if (freq_set(d, freq) < 0)
			WARN(d, "cannot set the clock's frequency: %s", strerror(errno));
		break;
	case SERVO_HOLD:
		break;
	}
	if (!d->clock_set) {
		d->clock_set = true;
		d->dirty = true;
		LOG(d, "clock in step with %s, offset %s", source_name(src), show(offset, buf, sizeof(buf)));
	}
	DEBUG(d, "%s: offset %s, frequency %+.0f ppb", source_name(src), show(offset, buf, sizeof(buf)),
	      -d->servo.freq);
}

/* A new source in use: the servo finds the rate again, keeping what it knows of it. */
static void source_changed(struct daemon *d, enum source before, enum source after)
{
	LOG(d, "time source: %s (was %s)", source_name(after), source_name(before));
	d->dirty = true;
	servo_reset(&d->servo);
}

/* A sample of @src, taken at monotonic time @at, good for @hold. */
static void on_sample(struct daemon *d, enum source src, ns_t offset, ns_t at, ns_t hold)
{
	enum source before = d->sel.cur;
	enum source after = select_sample(&d->sel, src, offset, mono_now(), hold);

	if (after != before)
		source_changed(d, before, after);
	if (after == src)
		discipline(d, src, offset, at);
	else
		DEBUG(d, "%s: offset %lld ns, not used (%s in use)", source_name(src), (long long)offset,
		      source_name(after));
}

/* The stamp of a frame: the kernel's when it came with the message, else the clock now. */
static ns_t rx_stamp(struct msghdr *mh)
{
	struct cmsghdr *c;

	for (c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c)) {
		ns_t t;

		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS &&
		    c->cmsg_len > CMSG_LEN(0) && stamp_parse(CMSG_DATA(c), c->cmsg_len - CMSG_LEN(0), &t))
			return t;
	}
	return real_now();
}

/* ---- NTP ---- */

static int ntp_open(struct daemon *d)
{
	struct addrinfo hints, *res;
	int one = 1;
	int fd;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;	/* the stick resolves no name */
	if (getaddrinfo(d->ntp_server, NTP_PORT, &hints, &res) != 0 || !res) {
		WARN(d, "NTP server %s is not an address", d->ntp_server);
		return -1;
	}
	memcpy(&d->ntp_addr, res->ai_addr, res->ai_addrlen);
	d->ntp_addrlen = res->ai_addrlen;
	fd = socket(res->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	freeaddrinfo(res);
	if (fd < 0)
		return -1;
	/* The kernel's stamp of an answer's arrival: the daemon may come to read it late. */
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
	d->ntp_fd = fd;
	return 0;
}

static void ntp_query(struct daemon *d, ns_t mono)
{
	uint8_t buf[NTP_PACKET_LEN];
	ns_t interval;

	if (d->ntp_waiting) {
		d->ntp_misses++;
		d->ntp_waiting = false;
		DEBUG(d, "NTP: no answer from %s", d->ntp_server);
	}
	d->ntp_t1 = real_now();
	ntp_request(buf, d->ntp_t1);
	d->ntp_sent_mono = mono;
	if (sendto(d->ntp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&d->ntp_addr, d->ntp_addrlen) < 0) {
		/* No route yet: the management link comes up after timed. Try again at the next poll. */
		DEBUG(d, "NTP: send to %s: %s", d->ntp_server, strerror(errno));
	} else {
		d->ntp_waiting = true;
	}
	d->ntp_polls++;
	interval = d->ntp_polls < 4 ? NTP_POLL_FIRST : d->ntp_polls < 12 ? NTP_POLL_NORMAL : NTP_POLL_STEADY;
	if (d->ntp_misses >= 3 && interval > NTP_POLL_NORMAL)
		interval = NTP_POLL_NORMAL;
	d->ntp_next = mono + interval;
}

static void ntp_read(struct daemon *d)
{
	uint8_t buf[128];
	uint8_t cbuf[128];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh;
	struct ntp_sample s;
	enum ntp_status st;
	ns_t t4, mono, at;
	ssize_t n;

	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cbuf;
	mh.msg_controllen = sizeof(cbuf);
	n = recvmsg(d->ntp_fd, &mh, 0);
	if (n < 0)
		return;
	t4 = rx_stamp(&mh);
	mono = mono_now();
	if (!d->ntp_waiting)
		return;
	st = ntp_answer(buf, (size_t)n, d->ntp_t1, t4, &s);
	if (st != NTP_OK) {
		DEBUG(d, "NTP: answer from %s refused: %s", d->ntp_server, ntp_status_name(st));
		if (st == NTP_KOD) {
			d->ntp_waiting = false;
			d->ntp_misses = 3;
			d->ntp_next = mono + NTP_POLL_STEADY;
		}
		return;
	}
	d->ntp_waiting = false;
	d->ntp_misses = 0;
	d->ntp_last = s;
	d->ntp_have_last = true;
	DEBUG(d, "NTP: answer, offset %lld ns, delay %lld ns", (long long)s.offset, (long long)s.delay);
	ntp_filter_add(&d->ntp_filter, &s, mono);
	/*
	 * A step makes the filter forget. An answer kept out for its delay is
	 * still an answer: the server stays good.
	 */
	if (!ntp_filter_take(&d->ntp_filter, mono, 10 * 60 * NS_PER_SEC, &s, &at)) {
		select_refresh(&d->sel, SRC_NTP, mono, 3 * NTP_POLL_STEADY);
		return;
	}
	on_sample(d, SRC_NTP, s.offset, at, 3 * NTP_POLL_STEADY);
}

/* ---- PTP ---- */

static void ptp_close(struct daemon *d)
{
	if (d->l2_fd >= 0)
		close(d->l2_fd);
	if (d->event_fd >= 0)
		close(d->event_fd);
	if (d->general_fd >= 0)
		close(d->general_fd);
	d->l2_fd = d->event_fd = d->general_fd = -1;
	d->ptp_ready = false;
	d->hw_ready = false;
}

/* ---- PTP with hardware stamps ---- */

/* The receive filters that stamp a Sync over layer 2. */
static bool l2_filter(int filter)
{
	switch (filter) {
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L2_SYNC:
		return true;
	default:
		return false;
	}
}

/*
 * timed -H: turn on the hardware stamps of @iface, transmit and the layer-2
 * PTP receive filter (SIOCSHWTSTAMP). That takes CAP_NET_ADMIN, which the
 * daemon does not have; root runs this before it. Return the exit status.
 */
static int hw_enable(const char *iface)
{
	struct hwtstamp_config cfg;
	struct ifreq ifr;
	int fd, err = 0;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("timed: socket");
		return 1;
	}
	/* One-step Syncs, for a master, and two-step stamps of the rest; or two-step stamps alone. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.tx_type = HWTSTAMP_TX_ONESTEP_SYNC;
	cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
	ifr.ifr_data = (void *)&cfg;
	if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
		cfg.tx_type = HWTSTAMP_TX_ON;
		cfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_L2_EVENT;
		if (ioctl(fd, SIOCSHWTSTAMP, &ifr) < 0) {
			err = errno;
			fprintf(stderr, "timed: %s: no hardware stamps: %s\n", iface, strerror(err));
		}
	}
	close(fd);
	return err ? 1 : 0;
}

/* The PHC's frequency, in the servo's parts per billion. */
static double phc_freq_now(const struct daemon *d)
{
	struct timex tx;

	memset(&tx, 0, sizeof(tx));
	if (clock_adjtime(d->phc, &tx) < 0)
		return 0;
	return (double)tx.freq / PPB_TO_FREQ;
}

static int phc_freq_set(const struct daemon *d, double ppb)
{
	struct timex tx;

	if (d->dry)
		return 0;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY;
	tx.freq = (long)(ppb * PPB_TO_FREQ);
	return clock_adjtime(d->phc, &tx) < 0 ? -1 : 0;
}

/* Step the PHC by -@offset: it adds an offset to the running clock, losing nothing. */
static bool phc_step(struct daemon *d, ns_t offset)
{
	ns_t by = -offset;
	struct timex tx;
	char buf[32];

	if (!d->dry) {
		memset(&tx, 0, sizeof(tx));
		tx.modes = ADJ_SETOFFSET | ADJ_NANO;
		tx.time.tv_sec = (time_t)(by / NS_PER_SEC);
		tx.time.tv_usec = (long)(by % NS_PER_SEC);
		if (tx.time.tv_usec < 0) {
			tx.time.tv_usec += NS_PER_SEC;
			tx.time.tv_sec -= 1;
		}
		if (clock_adjtime(d->phc, &tx) < 0) {
			WARN(d, "cannot step the PHC: %s", strerror(errno));
			return false;
		}
	}
	d->phc_steps++;
	d->dirty = true;
	/* What was stamped in the PHC before is of another clock. */
	ptp_clock_stepped(&d->ptp);
	median3_reset(&d->phc_med);
	LOG(d, "PHC stepped by %s", show(by, buf, sizeof(buf)));
	return true;
}

/*
 * Take the hardware stamps of the layer-2 socket, when the interface gives
 * them and has them on: a PHC, transmit stamps and a receive filter for PTP
 * over layer 2 (set by timed -H). Otherwise the software stamps stay.
 */
static void hw_open(struct daemon *d)
{
	struct ethtool_ts_info info;
	struct hwtstamp_config cfg;
	const char *why = NULL;
	struct ifreq ifr;
	int flags;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", d->iface);
	memset(&info, 0, sizeof(info));
	info.cmd = ETHTOOL_GET_TS_INFO;
	ifr.ifr_data = (void *)&info;
	memset(&cfg, 0, sizeof(cfg));
	if (ioctl(d->l2_fd, SIOCETHTOOL, &ifr) < 0 || info.phc_index < 0 ||
	    !(info.so_timestamping & SOF_TIMESTAMPING_TX_HARDWARE) ||
	    !(info.so_timestamping & SOF_TIMESTAMPING_RX_HARDWARE)) {
		why = "the interface gives none";
		goto out;
	}
	ifr.ifr_data = (void *)&cfg;
	if (ioctl(d->l2_fd, SIOCGHWTSTAMP, &ifr) < 0 ||
	    (cfg.tx_type != HWTSTAMP_TX_ON && cfg.tx_type != HWTSTAMP_TX_ONESTEP_SYNC) ||
	    !l2_filter(cfg.rx_filter)) {
		why = "not turned on (timed -H)";
		goto out;
	}
	d->hw_onestep = cfg.tx_type == HWTSTAMP_TX_ONESTEP_SYNC;
	if (d->phc_fd < 0 || d->phc_index != info.phc_index) {
		char path[32];
		double freq;

		if (d->phc_fd >= 0)
			close(d->phc_fd);
		snprintf(path, sizeof(path), "/dev/ptp%d", info.phc_index);
		d->phc_fd = open(path, (d->dry ? O_RDONLY : O_RDWR) | O_CLOEXEC);
		if (d->phc_fd < 0) {
			why = strerror(errno);
			goto out;
		}
		d->phc_index = info.phc_index;
		d->phc = FD_TO_CLOCKID(d->phc_fd);
		d->phc_set = false;
		d->phc_owner = PHC_FREE;
		d->pon_ready = false;
		d->pon_retry_at = 0;
		freq = phc_freq_now(d);
		servo_init(&d->phc_servo, STEP_LIMIT, freq, freq != 0);
		if (d->phc_servo.freq != freq)
			phc_freq_set(d, d->phc_servo.freq);
	}
	flags = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RX_HARDWARE |
		SOF_TIMESTAMPING_RAW_HARDWARE | SOF_TIMESTAMPING_OPT_TSONLY;
	if (setsockopt(d->l2_fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
		why = strerror(errno);
		goto out;
	}
	d->hw_ready = true;
out:
	if (d->hw_ready)
		LOG(d, "PTP stamps: hardware, PHC /dev/ptp%d%s", d->phc_index,
		    d->hw_onestep ? ", one-step Sync" : "");
	else
		LOG(d, "PTP stamps: software (hardware: %s)", why);
}

/* The hardware stamp of a frame, when it came with one. */
static bool rx_stamp_hw(struct msghdr *mh, ns_t *t)
{
	struct cmsghdr *c;

	for (c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == (int)SO_TIMESTAMPING &&
		    c->cmsg_len > CMSG_LEN(0) && stamp_parse_hw(CMSG_DATA(c), c->cmsg_len - CMSG_LEN(0), t))
			return true;
	return false;
}

/*
 * The error queue of the layer-2 socket: the time a Delay_Req left, stamped
 * by the interface. The stamp comes without the message (OPT_TSONLY); the
 * driver stamps one message at a time and timed sends only Delay_Req, so it
 * is the last one's.
 */
static void hw_tx_read(struct daemon *d)
{
	bool any = false;

	for (;;) {
		uint8_t cbuf[256];
		struct msghdr mh;
		ns_t t3;

		memset(&mh, 0, sizeof(mh));
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);
		if (recvmsg(d->l2_fd, &mh, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
			break;
		any = true;
		if (rx_stamp_hw(&mh, &t3)) {
			d->tx_stamps++;
			ptp_delay_req_sent(&d->ptp, t3);
		}
	}
	/* Nothing queued: an error of the socket itself, which reading clears; then open again. */
	if (!any) {
		int err = 0;
		socklen_t len = sizeof(err);

		if (getsockopt(d->l2_fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err) {
			WARN(d, "PTP: %s: %s", d->iface, strerror(err));
			ptp_close(d);
			d->ptp_reopen_at = mono_now() + REOPEN_PERIOD;
		}
	}
}

/*
 * The PHC is steered by @owner from now: the offsets filtered before are of
 * the other one's reference, and the rate the servo knows is the crystal's.
 */
static void phc_take(struct daemon *d, enum phc_owner owner)
{
	if (d->phc_owner == owner)
		return;
	d->phc_owner = owner;
	d->dirty = true;
	median3_reset(&d->phc_med);
	servo_reset(&d->phc_servo);
	/* A rate no crystal runs at, which one owner left, is not the next one's to start from. */
	if (d->phc_servo.drift > PHC_SANE_PPB || d->phc_servo.drift < -PHC_SANE_PPB)
		servo_init(&d->phc_servo, STEP_LIMIT, 0, false);
	if (owner != PHC_PON)
		d->pon_in_step = false;
	if (owner != PHC_SYS)
		d->phc_sys_in_step = false;
}

/*
 * An offset of the PHC from the master: the PHC's servo, as discipline() is
 * the system clock's. The stamps are the PHC's but the master's are only as
 * good as the master: one that stamps in software gave offsets some 20 us
 * apart, and stalls. They go through a median of three, as the software
 * stamps do, and the servo takes the gains that hold with its delay: the
 * hard gains of linuxptp swung to the 500 ppm limit either way (measured).
 */
static void phc_sample(struct daemon *d, ns_t offset, ns_t mono)
{
	double freq = 0;
	char buf[32];
	ns_t off;

	phc_take(d, PHC_PTP);
	off = median3_add(&d->phc_med, offset);
	/* Three to take the median of, but a clock far out is set at once. */
	if (d->phc_med.n < 3 && offset < STEP_LIMIT && offset > -STEP_LIMIT)
		return;
	d->phc_servo.baseline = SERVO_BASELINE_PTP;
	d->phc_servo.gains = SERVO_GAINS_FILTERED;
	switch (servo_sample(&d->phc_servo, off, mono, &freq)) {
	case SERVO_STEP:
		if (phc_step(d, off))
			d->phc_set = true;
		return;
	case SERVO_JUMP:
		if (phc_step(d, off) && phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		d->phc_set = true;
		return;
	case SERVO_FREQ:
		if (phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		break;
	case SERVO_HOLD:
		break;
	}
	if (!d->phc_set) {
		d->phc_set = true;
		d->dirty = true;
		LOG(d, "PHC in step with the master, offset %s", show(off, buf, sizeof(buf)));
	}
	DEBUG(d, "PHC: offset %lld ns, frequency %+.0f ppb", (long long)off, -d->phc_servo.freq);
}

/*
 * The system clock less the PHC: the PHC is read between two readings of the
 * system clock, five times (PTP_SYS_OFFSET), and the closest pair taken.
 */
static bool phc_read_sys(struct daemon *d, ns_t *offset, ns_t *window)
{
	struct ptp_sys_offset req;
	struct phc_reading r;
	unsigned int i;

	memset(&req, 0, sizeof(req));
	req.n_samples = PHC_SAMPLES;
	if (ioctl(d->phc_fd, PTP_SYS_OFFSET, &req) < 0) {
		DEBUG(d, "PHC: cannot read it against the system clock: %s", strerror(errno));
		return false;
	}
	r.n = req.n_samples;
	for (i = 0; i <= 2 * r.n; i++) {
		ns_t t = (ns_t)req.ts[i].sec * NS_PER_SEC + req.ts[i].nsec;

		if (i % 2)
			r.phc[i / 2] = t;
		else
			r.sys[i / 2] = t;
	}
	return phc_sys_offset(&r, offset, window);
}

/*
 * The system clock against the PHC, whose time is @src's (a master's or the
 * OLT's) on a timescale @shift ahead of UTC, as a sample of @src good for
 * @hold.
 */
static void phc_follow(struct daemon *d, ns_t mono, enum source src, ns_t shift, ns_t hold)
{
	ns_t offset, window;

	if (!phc_read_sys(d, &offset, &window))
		return;
	offset += shift;
	d->sys_phc_offset = offset;
	d->sys_phc_window = window;
	d->have_sys_phc = true;
	on_sample(d, src, offset, mono, hold);
}

/* ---- the OLT's seconds ---- */

/* A master heard over layer 2 with hardware stamps steers the PHC. */
/*
 * A master heard, and better than this clock as a master would be (-M): the
 * one the slave side follows.
 */
static bool ptp_master_better(const struct daemon *d, ns_t mono)
{
	return d->ptp.have_master && ptp_master_alive(&d->ptp, mono) &&
	       (!d->master_offer || ptp_dataset_cmp(&d->ptp.master_ds, &d->master.ds) < 0);
}

static bool phc_ptp_steers(const struct daemon *d, ns_t mono)
{
	return d->hw_ready && d->ptp.local_timescale && mono - d->l2_sync_at < PHC_L2_HOLD &&
	       ptp_master_better(d, mono);
}

/*
 * What is to steer the PHC: a master heard over layer 2 with hardware stamps;
 * else the OLT's seconds once the PHC holds the OLT's time of day; nothing
 * while a time of day the OLT gave waits to be taken, so that the PHC jumps
 * to it cleanly and no servo takes the jump for an offset to steer away;
 * else, for a master whose time is the system clock's, the system clock;
 * else the OLT's seconds, for their rate alone.
 */
static enum phc_owner phc_want(const struct daemon *d, ns_t mono)
{
	bool pon = d->pon_ready && mono - d->pon_at < PON_HOLD;

	if (phc_ptp_steers(d, mono))
		return PHC_PTP;
	if (pon && d->pon_tod_set)
		return PHC_PON;
	if (pon && d->tod_have && mono - d->tod.at <= PON_TOD_WAIT)
		return PHC_FREE;
	if (d->master_offer && d->clock_set && (d->sel.cur == SRC_NTP || d->sel.cur == SRC_OLT))
		return PHC_SYS;
	return pon ? PHC_PON : PHC_FREE;
}

/*
 * Ask the PHC for the OLT's seconds, its external timestamps of channel 0.
 * A PHC without them, or with no PON behind it, refuses; it is asked again
 * a minute later.
 */
static void pon_open(struct daemon *d)
{
	struct ptp_extts_request req;

	memset(&req, 0, sizeof(req));
	req.index = 0;
	req.flags = PTP_ENABLE_FEATURE | PTP_RISING_EDGE;
	if (ioctl(d->phc_fd, PTP_EXTTS_REQUEST2, &req) < 0) {
		if (!d->pon_none_said)
			LOG(d, "OLT seconds: none from /dev/ptp%d (%s)", d->phc_index, strerror(errno));
		d->pon_none_said = true;
		return;
	}
	d->pon_ready = true;
	d->pon_none_said = false;
	pon_reset(&d->pon);
	d->dirty = true;
	LOG(d, "OLT seconds: from /dev/ptp%d", d->phc_index);
}

/*
 * An offset of the PHC from the OLT's seconds: the PHC's servo, as for a
 * master. The seconds are as exact as the interrupt that reads them, within
 * some 6 us (measured): through a median of three, with the gains of the
 * master's offsets, which hold with its delay.
 */
static void pon_steer(struct daemon *d, ns_t offset, ns_t mono)
{
	double freq = 0;
	char buf[32];
	ns_t off;

	phc_take(d, PHC_PON);
	/*
	 * The median once there are three, the offset itself before: a PHC
	 * whose rate is far out jumps under the seconds every few seconds, and
	 * must still be steered between the jumps.
	 */
	off = median3_add(&d->phc_med, offset);
	d->pon_offset = off;
	d->pon_have_offset = true;
	d->phc_servo.baseline = SERVO_BASELINE_PTP;
	d->phc_servo.gains = SERVO_GAINS_FILTERED;
	switch (servo_sample(&d->phc_servo, off, mono, &freq)) {
	case SERVO_STEP:
		phc_step(d, off);
		return;
	case SERVO_JUMP:
		if (phc_step(d, off) && phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		return;
	case SERVO_FREQ:
		if (phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		break;
	case SERVO_HOLD:
		break;
	}
	if (!d->pon_in_step) {
		d->pon_in_step = true;
		d->dirty = true;
		LOG(d, "PHC in step with the OLT's seconds, offset %s", show(off, buf, sizeof(buf)));
	}
	DEBUG(d, "PHC: offset %lld ns from the OLT's seconds, frequency %+.0f ppb", (long long)off,
	      -d->phc_servo.freq);
}

/* The PHC holds the time of day the OLT gave: it is a source for the system clock. */
static void pon_tod_taken(struct daemon *d, const char *why)
{
	d->pon_tod_set = true;
	d->dirty = true;
	LOG(d, "PHC on the OLT's time of day, frame %u (%s)", d->tod.superframe, why);
}

/* A second of the OLT's that began at the PHC's time @t. */
static void pon_event(struct daemon *d, ns_t t)
{
	ns_t mono = mono_now();
	ns_t off;

	d->pon_at = mono;
	switch (pon_second(&d->pon, t, &off)) {
	case PON_ANCHOR:
		return;
	case PON_JUMPED:
		d->pon_jumps++;
		d->dirty = true;
		if (d->phc_owner == PHC_PON)
			median3_reset(&d->phc_med);
		LOG(d, "OLT seconds: the PHC jumped; counting from this second");
		/* After omcid told of a time of day, the jump is the PHC taking it. */
		if (d->tod_have && !d->pon_tod_set && mono > d->tod.at)
			pon_tod_taken(d, "the PHC jumped to it");
		return;
	case PON_OFFSET:
		break;
	}
	d->pon_seconds++;
	if (phc_want(d, mono) == PHC_PON)
		pon_steer(d, off, mono);
}

/*
 * The seconds the PHC has queued, once poll() said it has some: a read of the
 * device waits for one whatever O_NONBLOCK says, so it is read once.
 */
static void pon_read(struct daemon *d)
{
	struct ptp_extts_event ev[4];
	ssize_t n = read(d->phc_fd, ev, sizeof(ev));
	size_t i;

	if (n < 0) {
		WARN(d, "OLT seconds: %s", strerror(errno));
		d->pon_ready = false;
		d->pon_retry_at = mono_now() + PON_RETRY;
		return;
	}
	for (i = 0; i < (size_t)n / sizeof(ev[0]); i++)
		if (ev[i].index == 0)
			pon_event(d, (ns_t)ev[i].t.sec * NS_PER_SEC + ev[i].t.nsec);
}

/*
 * The PHC on the system clock, for a master whose time is the system clock's:
 * the PHC less the system clock's time on the PTP timescale, read exactly
 * (PTP_SYS_OFFSET), to the PHC's servo with the gains of an exact reading.
 */
static void phc_on_sys(struct daemon *d, ns_t mono)
{
	ns_t sys_less_phc, window, off;
	double freq = 0;
	char buf[32];

	if (!phc_read_sys(d, &sys_less_phc, &window))
		return;
	phc_take(d, PHC_SYS);
	off = -sys_less_phc - (ns_t)PTP_UTC_OFFSET_DEFAULT * NS_PER_SEC;
	d->phc_servo.baseline = SERVO_BASELINE_PTP;
	d->phc_servo.gains = SERVO_GAINS_HARD;
	switch (servo_sample(&d->phc_servo, off, mono, &freq)) {
	case SERVO_STEP:
		phc_step(d, off);
		return;
	case SERVO_JUMP:
		if (phc_step(d, off) && phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		return;
	case SERVO_FREQ:
		if (phc_freq_set(d, freq) < 0)
			WARN(d, "cannot set the PHC's frequency: %s", strerror(errno));
		break;
	case SERVO_HOLD:
		break;
	}
	if (!d->phc_sys_in_step) {
		d->phc_sys_in_step = true;
		d->dirty = true;
		LOG(d, "PHC in step with the system clock, offset %s", show(off, buf, sizeof(buf)));
	}
}

static int udp_socket(const struct daemon *d, unsigned int port, struct in_addr group)
{
	struct ip_mreqn mr;
	struct sockaddr_in sa;
	int one = 1, zero = 0, ttl = 1;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, d->iface, (socklen_t)strlen(d->iface) + 1) < 0)
		goto fail;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		goto fail;
	memset(&mr, 0, sizeof(mr));
	mr.imr_multiaddr = group;
	mr.imr_ifindex = d->ifindex;
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0)
		goto fail;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof(mr));
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &zero, sizeof(zero));
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
	return fd;
fail:
	close(fd);
	return -1;
}

static int ptp_open(struct daemon *d)
{
	struct sockaddr_ll sll;
	struct packet_mreq pm;
	struct in_addr group;
	struct ifreq ifr;
	int one = 1;
	int udp_err;
	int s;

	d->ifindex = (int)if_nametoindex(d->iface);
	if (!d->ifindex)
		return -1;

	/* The clock identity of this node, from the interface's MAC. */
	s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (s < 0)
		return -1;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", d->iface);
	if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
		close(s);
		return -1;
	}
	close(s);
	{
		uint8_t id[8];

		ptp_clock_id_from_mac(id, (const uint8_t *)ifr.ifr_hwaddr.sa_data);
		/* A new identity restarts the slave; the same one leaves what it has learned. */
		if (memcmp(id, d->ptp.own.clock, 8)) {
			ptp_slave_init(&d->ptp, d->domain, id);
			ptp_master_init(&d->master, d->domain, id);
		}
	}

	d->l2_fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_1588));
	if (d->l2_fd >= 0) {
		memset(&sll, 0, sizeof(sll));
		sll.sll_family = AF_PACKET;
		sll.sll_protocol = htons(ETH_P_1588);
		sll.sll_ifindex = d->ifindex;
		memset(&pm, 0, sizeof(pm));
		pm.mr_ifindex = d->ifindex;
		pm.mr_type = PACKET_MR_MULTICAST;
		pm.mr_alen = 6;
		memcpy(pm.mr_address, ptp_l2_group, 6);
		if (bind(d->l2_fd, (struct sockaddr *)&sll, sizeof(sll)) < 0 ||
		    setsockopt(d->l2_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &pm, sizeof(pm)) < 0) {
			close(d->l2_fd);
			d->l2_fd = -1;
		} else {
			setsockopt(d->l2_fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
			hw_open(d);
		}
	}

	inet_pton(AF_INET, PTP_UDP_GROUP, &group);
	d->event_fd = udp_socket(d, PTP_EVENT_PORT, group);
	udp_err = d->event_fd < 0 ? errno : 0;
	d->general_fd = udp_socket(d, PTP_GENERAL_PORT, group);
	if (d->general_fd < 0 && !udp_err)
		udp_err = errno;

	if (d->l2_fd < 0 && (d->event_fd < 0 || d->general_fd < 0)) {
		ptp_close(d);
		return -1;
	}
	d->ptp_ready = true;
	LOG(d, "PTP on %s, domain %u: layer 2 %s, UDP %s", d->iface, d->domain, d->l2_fd >= 0 ? "yes" : "no",
	    udp_err ? strerror(udp_err) : "yes");
	return 0;
}

static void ptp_send_delay_req(struct daemon *d)
{
	uint8_t buf[PTP_DELAY_REQ_LEN];
	size_t len;

	if (d->ptp.local_timescale) {
		/* The interface stamps it as it leaves; the stamp comes on the error queue. */
		if (d->ptp.request_wait && !d->ptp.request_t3_known)
			d->tx_missed++;
		len = ptp_delay_req_later(&d->ptp, buf);
	} else {
		/* The time of sending is taken before the send, so that the message carries it back. */
		len = ptp_delay_req(&d->ptp, buf, real_now());
	}
	if (!len)
		return;
	if (d->via == VIA_L2 && d->l2_fd >= 0) {
		struct sockaddr_ll sll;

		memset(&sll, 0, sizeof(sll));
		sll.sll_family = AF_PACKET;
		sll.sll_protocol = htons(ETH_P_1588);
		sll.sll_ifindex = d->ifindex;
		sll.sll_halen = 6;
		memcpy(sll.sll_addr, ptp_l2_group, 6);
		if (sendto(d->l2_fd, buf, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
			DEBUG(d, "PTP: Delay_Req over layer 2: %s", strerror(errno));
	} else if (d->event_fd >= 0) {
		struct sockaddr_in sa;

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(PTP_EVENT_PORT);
		inet_pton(AF_INET, PTP_UDP_GROUP, &sa.sin_addr);
		if (sendto(d->event_fd, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0)
			DEBUG(d, "PTP: Delay_Req over UDP: %s", strerror(errno));
	}
}

/* ---- the master ---- */

static void l2_send(struct daemon *d, const uint8_t *buf, size_t len, const char *what)
{
	struct sockaddr_ll sll;

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_1588);
	sll.sll_ifindex = d->ifindex;
	sll.sll_halen = 6;
	memcpy(sll.sll_addr, ptp_l2_group, 6);
	if (sendto(d->l2_fd, buf, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0)
		DEBUG(d, "PTP: %s: %s", what, strerror(errno));
}

/* Whether the PHC holds a time worth giving: the OLT's, or the system clock's. */
static bool phc_has_time(const struct daemon *d)
{
	if (d->phc_owner == PHC_PON)
		return d->pon_tod_set && d->pon_in_step;
	return d->phc_owner == PHC_SYS && d->phc_sys_in_step;
}

/* What this clock says of itself as a master, from where its time comes. */
static void master_quality(struct daemon *d)
{
	struct ptp_master *m = &d->master;

	if (d->phc_owner == PHC_PON) {
		m->ds.accuracy = PTP_ACCURACY_10US;
		m->time_source = PTP_TIME_SOURCE_OTHER;
	} else if (d->sel.cur == SRC_NTP) {
		m->ds.accuracy = PTP_ACCURACY_1MS;
		m->time_source = PTP_TIME_SOURCE_NTP;
	} else {
		m->ds.accuracy = PTP_ACCURACY_1S;
		m->time_source = PTP_TIME_SOURCE_OTHER;
	}
}

/*
 * The master (-M), once a second: master while the PHC holds a time worth
 * giving, the interface writes the time into a Sync as it leaves, and no
 * better clock is heard (the slave side keeps the best one heard); then an
 * Announce and a one-step Sync.
 */
static void master_tick(struct daemon *d, ns_t mono)
{
	bool may = d->hw_ready && d->hw_onestep && d->l2_fd >= 0 && phc_has_time(d);
	uint8_t buf[PTP_ANNOUNCE_LEN];
	struct timespec ts;
	bool now;

	if (may)
		master_quality(d);
	now = may && !ptp_master_better(d, mono);
	if (now != d->is_master) {
		d->is_master = now;
		d->dirty = true;
		d->master_next_at = mono;
		if (now)
			LOG(d, "PTP: master on %s, %s time", d->iface,
			    d->phc_owner == PHC_PON ? "the OLT's" : "the system clock's");
		else
			LOG(d, "PTP: master no more (%s)", may ? "a better master is heard" : "no time to give");
	}
	if (!d->is_master || mono < d->master_next_at)
		return;
	d->master_next_at = mono + MASTER_PERIOD;
	if (clock_gettime(d->phc, &ts) < 0)
		return;
	l2_send(d, buf, ptp_announce_build(&d->master, buf, (ns_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec),
		"Announce");
	l2_send(d, buf, ptp_sync_build(&d->master, buf), "Sync");
	d->syncs_sent++;
}

/* A Delay_Req to this master: the Delay_Resp, with the time it came by the interface's stamp. */
static void master_delay_req(struct daemon *d, const uint8_t *req, size_t len, struct msghdr *mh)
{
	uint8_t resp[PTP_DELAY_RESP_LEN];
	size_t n;
	ns_t t4;

	if (!rx_stamp_hw(mh, &t4))
		return;
	n = ptp_delay_resp_build(&d->master, req, len, t4, resp);
	if (!n)
		return;
	l2_send(d, resp, n, "Delay_Resp");
	d->delay_resps_sent++;
}

static void ptp_read(struct daemon *d, int fd, enum transport via)
{
	uint8_t buf[256];
	uint8_t cbuf[128];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh;
	struct ptp_result r;
	ssize_t n;
	ns_t rx, mono, off;

	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	mh.msg_control = cbuf;
	mh.msg_controllen = sizeof(cbuf);
	n = recvmsg(fd, &mh, 0);
	if (n <= 0) {
		if (n < 0 && errno != EAGAIN && errno != EINTR) {
			/* The interface went away or down: close, and open again later. */
			WARN(d, "PTP: %s: %s", d->iface, strerror(errno));
			ptp_close(d);
			d->ptp_reopen_at = mono_now() + REOPEN_PERIOD;
		}
		return;
	}
	rx = rx_stamp(&mh);
	mono = mono_now();
	if (d->is_master && via == VIA_L2 && (buf[0] & 0x0f) == PTP_DELAY_REQ) {
		master_delay_req(d, buf, (size_t)n, &mh);
		return;
	}
	if (d->hw_ready && (buf[0] & 0x0f) != PTP_ANNOUNCE) {
		bool hw = via == VIA_L2;

		/*
		 * Over layer 2 a Sync is taken with its hardware stamp, or
		 * not at all; over UDP, while the master is heard over layer 2,
		 * not at all either: the two kinds of stamp are of two clocks.
		 */
		if (hw && (buf[0] & 0x0f) == PTP_SYNC) {
			if (!rx_stamp_hw(&mh, &rx))
				return;
			d->l2_sync_at = mono;
		}
		if (!hw && mono - d->l2_sync_at < PHC_L2_HOLD)
			return;
		ptp_set_local_timescale(&d->ptp, hw);
	} else if (!d->hw_ready) {
		ptp_set_local_timescale(&d->ptp, false);
	}
	if (d->verbose && (buf[0] & 0x0f) <= PTP_SYNC)
		DEBUG(d, "PTP: Sync seq %u received at %lld, now %lld", get16(buf + 30), (long long)rx,
		      (long long)real_now());
	if (!ptp_rx(&d->ptp, buf, (size_t)n, rx, mono, &r))
		return;
	if (r.ev == PTP_EV_MASTER) {
		const uint8_t *c = d->ptp.master.clock;

		d->via = via;
		d->delay_req_at = mono + NS_PER_SEC;
		median3_reset(&d->ptp_med);
		median3_reset(&d->phc_med);
		servo_reset(&d->phc_servo);
		LOG(d, "PTP master %02x%02x%02x%02x%02x%02x%02x%02x over %s, class %u, UTC offset %d s", c[0],
		    c[1], c[2], c[3], c[4], c[5], c[6], c[7], via == VIA_L2 ? "layer 2" : "UDP",
		    d->ptp.master_ds.clock_class, d->ptp.utc_offset);
	} else if (r.ev == PTP_EV_OFFSET) {
		int log2 = d->ptp.sync_log_interval;

		/* A master worse than this one, which is master: not followed. */
		if (!ptp_master_better(d, mono))
			return;
		ns_t hold = 4 * (log2 > 0 ? NS_PER_SEC << log2 : NS_PER_SEC);

		/* The way the master's Syncs come now, which its Delay_Req follow. */
		d->via = via;

		if (hold < 8 * NS_PER_SEC)
			hold = 8 * NS_PER_SEC;
		d->ptp_last_offset = r.offset;
		d->ptp_have_offset = true;
		/* With hardware stamps the offset is the PHC's; the system clock follows the PHC. */
		if (d->ptp.local_timescale) {
			phc_sample(d, r.offset, mono);
			return;
		}
		off = median3_add(&d->ptp_med, r.offset);
		/* The servo takes the median once there are three to take it of. */
		if (d->ptp_med.n >= 3)
			on_sample(d, SRC_PTP, off, mono, hold);
	}
}

static void ptp_tick(struct daemon *d, ns_t mono)
{
	if (!d->ptp_ready) {
		if (mono >= d->ptp_reopen_at) {
			d->ptp_reopen_at = mono + REOPEN_PERIOD;
			ptp_open(d);
		}
		return;
	}
	if (d->ptp.have_master && !ptp_master_alive(&d->ptp, mono)) {
		LOG(d, "PTP master lost");
		ptp_master_lost(&d->ptp);
		d->ptp_have_offset = false;
		median3_reset(&d->ptp_med);
	}
	if (d->phc_set && phc_ptp_steers(d, mono) && mono >= d->phc_follow_at) {
		d->phc_follow_at = mono + PHC_FOLLOW_PERIOD;
		phc_follow(d, mono, SRC_PTP, ptp_utc_shift(&d->ptp), PHC_L2_HOLD);
	}
	if (ptp_master_better(d, mono) && mono >= d->delay_req_at) {
		ptp_send_delay_req(d);
		d->delay_req_at = mono + (d->ptp.delay_known ? PTP_DELAY_REQ_NORMAL : PTP_DELAY_REQ_FIRST);
	}
}

/* ---- the OLT ---- */

/* The file once a second: a new time when omcid wrote another. */
static void olt_check(struct daemon *d, ns_t mono)
{
	char text[128];
	struct stat st;
	ns_t utc, at;
	ssize_t n;
	int fd;

	if (mono < d->olt_check_at)
		return;
	d->olt_check_at = mono + OLT_PERIOD;
	if (stat(d->olt_path, &st) < 0)
		return;
	if (st.st_mtim.tv_sec == d->olt_mtime.tv_sec && st.st_mtim.tv_nsec == d->olt_mtime.tv_nsec &&
	    st.st_size == d->olt_size)
		return;
	d->olt_mtime = st.st_mtim;
	d->olt_size = st.st_size;
	fd = open(d->olt_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, text, sizeof(text));
	close(fd);
	if (n <= 0 || !olt_parse(text, (size_t)n, &utc, &at)) {
		WARN(d, "OLT: %s does not give a time", d->olt_path);
		return;
	}
	if (d->olt_have && utc == d->olt_utc && at == d->olt_at)
		return;
	d->olt_have = true;
	d->olt_utc = utc;
	d->olt_at = at;
	LOG(d, "OLT gave the time %lld", (long long)(utc / NS_PER_SEC));
	d->dirty = true;
	mono = mono_now();
	on_sample(d, SRC_OLT, olt_offset(utc, at, real_now(), mono), mono, OLT_HOLD);
}

/* The time of day omcid told, once a second: a new one when it wrote another. */
static void tod_check(struct daemon *d, ns_t mono)
{
	char text[160];
	struct pon_tod t;
	struct stat st;
	ssize_t n;
	int fd;

	if (mono < d->tod_check_at)
		return;
	d->tod_check_at = mono + OLT_PERIOD;
	if (stat(d->tod_path, &st) < 0)
		return;
	if (st.st_mtim.tv_sec == d->tod_mtime.tv_sec && st.st_mtim.tv_nsec == d->tod_mtime.tv_nsec &&
	    st.st_size == d->tod_size)
		return;
	d->tod_mtime = st.st_mtim;
	d->tod_size = st.st_size;
	fd = open(d->tod_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	n = read(fd, text, sizeof(text));
	close(fd);
	if (n <= 0 || !pon_tod_parse(text, (size_t)n, &t)) {
		WARN(d, "OLT: %s does not give a time of day", d->tod_path);
		return;
	}
	if (d->tod_have && t.superframe == d->tod.superframe && t.tstamp == d->tod.tstamp && t.at == d->tod.at)
		return;
	d->tod_have = true;
	d->tod = t;
	d->dirty = true;
	LOG(d, "OLT gave the time of day %lld.%09lld for frame %u", (long long)(t.tstamp / NS_PER_SEC),
	    (long long)(t.tstamp % NS_PER_SEC), t.superframe);
}

/* ---- main ---- */

static void print_usage(void)
{
	fprintf(stderr, "usage: timed [-n SERVER] [-p IFACE] [-o FILE] [-t FILE] [-M] [-d DOMAIN] [-s FILE] [-f] [-v] [-D]\n"
			"       timed -H IFACE\n");
	exit(2);
}

static void tick(struct daemon *d, ns_t mono)
{
	enum source before = d->sel.cur;
	enum source after = select_pick(&d->sel, mono);

	if (after != before)
		source_changed(d, before, after);
	if (d->ntp_fd >= 0 && mono >= d->ntp_next)
		ntp_query(d, mono);
	if (d->ntp_waiting && mono - d->ntp_sent_mono > NTP_REPLY_WAIT) {
		d->ntp_waiting = false;
		d->ntp_misses++;
		DEBUG(d, "NTP: no answer from %s", d->ntp_server);
	}
	if (d->iface)
		ptp_tick(d, mono);
	if (d->phc_fd >= 0 && !d->pon_ready && !d->dry && mono >= d->pon_retry_at) {
		d->pon_retry_at = mono + PON_RETRY;
		pon_open(d);
	}
	if (d->olt_path)
		olt_check(d, mono);
	if (d->tod_path)
		tod_check(d, mono);
	/* No jump came: the PHC held a time within a millisecond of the OLT's already. */
	if (d->tod_have && !d->pon_tod_set && d->pon_ready && mono - d->pon_at < PON_HOLD &&
	    mono - d->tod.at > PON_TOD_WAIT)
		pon_tod_taken(d, "the PHC held it already");
	/* The system clock on the PHC that holds the OLT's time. */
	if (d->pon_tod_set && d->phc_owner == PHC_PON && d->pon_in_step && mono >= d->pon_follow_at) {
		d->pon_follow_at = mono + PHC_FOLLOW_PERIOD;
		phc_follow(d, mono, SRC_PON, (ns_t)PTP_UTC_OFFSET_DEFAULT * NS_PER_SEC, PON_HOLD + NS_PER_SEC);
	}
	/* The PHC on the system clock, for a master that gives the system clock's time. */
	if (d->phc_fd >= 0 && !d->dry && phc_want(d, mono) == PHC_SYS && mono >= d->phc_sys_at) {
		d->phc_sys_at = mono + PHC_FOLLOW_PERIOD;
		phc_on_sys(d, mono);
	}
	if (d->master_offer)
		master_tick(d, mono);
}

int main(int argc, char **argv)
{
	struct daemon d;
	struct sigaction sa;
	ns_t state_at = 0;
	int opt;

	memset(&d, 0, sizeof(d));
	d.state_path = DEFAULT_STATE;
	d.ntp_fd = d.l2_fd = d.event_fd = d.general_fd = d.phc_fd = -1;
	select_init(&d.sel);
	ntp_filter_init(&d.ntp_filter);
	{
		static const uint8_t none[8] = { 0 };

		ptp_slave_init(&d.ptp, 0, none);
	}

	while ((opt = getopt(argc, argv, "n:p:o:t:Md:s:fvDH:")) != -1) {
		switch (opt) {
		case 'H':
			return hw_enable(optarg);
		case 'n':
			d.ntp_server = optarg;
			break;
		case 'p':
			d.iface = optarg;
			break;
		case 'o':
			d.olt_path = optarg;
			break;
		case 't':
			d.tod_path = optarg;
			break;
		case 'M':
			d.master_offer = true;
			break;
		case 'd':
			d.domain = (uint8_t)atoi(optarg);
			break;
		case 's':
			d.state_path = optarg;
			break;
		case 'f':
			d.foreground = true;
			break;
		case 'v':
			d.verbose = true;
			break;
		case 'D':
			d.dry = true;
			break;
		default:
			print_usage();
		}
	}
	if (optind != argc || (!d.ntp_server && !d.iface && !d.olt_path && !d.tod_path) ||
	    (d.master_offer && !d.iface))
		print_usage();

	if (!d.foreground)
		openlog("timed", LOG_PID, LOG_DAEMON);
	{
		double freq = freq_start(&d);

		/* A frequency the kernel holds is one a timed before this one set: right, and kept. */
		servo_init(&d.servo, STEP_LIMIT, freq, freq != 0);
		if (d.servo.freq != freq)
			freq_set(&d, d.servo.freq);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (d.ntp_server && ntp_open(&d) < 0)
		return 1;
	LOG(&d, "start: NTP %s, PTP %s%s, OLT %s, time of day %s", d.ntp_server ? d.ntp_server : "off",
	    d.iface ? d.iface : "off", d.master_offer ? " (may be master)" : "", d.olt_path ? d.olt_path : "off",
	    d.tod_path ? d.tod_path : "off");

	while (!g_stop) {
		struct pollfd pfd[5];
		int idx_ntp = -1, idx_l2 = -1, idx_ev = -1, idx_gen = -1, idx_pon = -1, n = 0;
		ns_t mono = mono_now();

		memset(pfd, 0, sizeof(pfd));
		if (d.ntp_fd >= 0) {
			idx_ntp = n;
			pfd[n].fd = d.ntp_fd;
			pfd[n++].events = POLLIN;
		}
		if (d.l2_fd >= 0) {
			idx_l2 = n;
			pfd[n].fd = d.l2_fd;
			pfd[n++].events = POLLIN;
		}
		if (d.event_fd >= 0) {
			idx_ev = n;
			pfd[n].fd = d.event_fd;
			pfd[n++].events = POLLIN;
		}
		if (d.general_fd >= 0) {
			idx_gen = n;
			pfd[n].fd = d.general_fd;
			pfd[n++].events = POLLIN;
		}
		if (d.pon_ready) {
			idx_pon = n;
			pfd[n].fd = d.phc_fd;
			pfd[n++].events = POLLIN;
		}
		/* A tick each 200 ms is enough for every timer here. */
		if (poll(pfd, (nfds_t)n, 200) < 0 && errno != EINTR)
			break;

		if (idx_ntp >= 0 && (pfd[idx_ntp].revents & POLLIN))
			ntp_read(&d);
		if (idx_l2 >= 0 && (pfd[idx_l2].revents & POLLERR) && d.l2_fd >= 0 && d.hw_ready)
			hw_tx_read(&d);
		if (idx_l2 >= 0 && (pfd[idx_l2].revents & POLLIN) && d.l2_fd >= 0)
			ptp_read(&d, d.l2_fd, VIA_L2);
		if (idx_ev >= 0 && (pfd[idx_ev].revents & POLLIN) && d.event_fd >= 0)
			ptp_read(&d, d.event_fd, VIA_UDP);
		if (idx_gen >= 0 && (pfd[idx_gen].revents & POLLIN) && d.general_fd >= 0)
			ptp_read(&d, d.general_fd, VIA_UDP);
		if (idx_pon >= 0 && (pfd[idx_pon].revents & POLLIN) && d.pon_ready)
			pon_read(&d);

		mono = mono_now();
		tick(&d, mono);
		if (d.dirty || mono >= state_at) {
			write_state(&d, mono);
			d.dirty = false;
			state_at = mono + STATE_PERIOD;
		}
	}
	LOG(&d, "stop");
	write_state(&d, mono_now());
	return 0;
}
