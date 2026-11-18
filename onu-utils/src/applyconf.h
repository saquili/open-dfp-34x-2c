/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/* applyconf.h: what applyconf.c gives the other parts of onu-apply. */

#ifndef APPLYCONF_H
#define APPLYCONF_H

#include <limits.h>

#include "applyutil.h"
#include "settings.h"

#define MAX_SESSIONS SETTINGS_SESSIONS
#define MAX_IPOE SETTINGS_IPOE
#define MAX_LANS SETTINGS_LANS
/* The rules the firewall's files carry, by their place. */
#define MAX_RULES SETTINGS_RULES
/*
 * The stick's own address on a link to the host, from which its DHCP server
 * answers; the host is given the concentrator's address as its gateway.
 */
#define LINK_ADDRESS "169.254.0.1"

/*
 * The files it reads and writes, under ONU_ROOT. LIB holds the helpers of
 * this tree that only other programs run, out of a person's way (ADR 0012):
 * DROP starts a program as an account of its own with the powers it needs,
 * and CALL tells onu-apply of a change, for a program that is not root's.
 * APPLIED holds the settings last applied, the data path's and the sessions'
 * accounts; while a change is on trial (TRIAL), KEPT the ones it goes back to;
 * PENDING a change onu-config change wrote, which try puts in force under the
 * lock. CONF is the settings in use: CONF_MAIN, /run/onu.conf, or CONF_MODE,
 * those the shape in force came up with.
 */
extern char RUN[PATH_MAX], CONF_MAIN[PATH_MAX], IDENTITY[PATH_MAX], SESSIONS[PATH_MAX];
extern char IPOE_DIR[PATH_MAX], NET[PATH_MAX], FORWARD[PATH_MAX], FORWARD6[PATH_MAX];
extern char LIB[PATH_MAX], DROP[PATH_MAX], CALL[PATH_MAX], SVC_DIR[PATH_MAX];
extern char LOCK[PATH_MAX], AGAIN[PATH_MAX];
extern char APPLIED[PATH_MAX], KEPT[PATH_MAX], PENDING[PATH_MAX], TRIAL[PATH_MAX];
extern const char *CONF;
extern char CONF_MODE[PATH_MAX];
/* Seconds a change stays on trial, TRIAL_LIMIT (90). */
extern long g_trial_limit;
/* Tenths of a second to wait for omcid to say that it changed its mode (50). */
extern long g_applied_tenths;
/* "bridge" or "passthrough": the shape the settings make (load_mode()). */
extern char g_mode[16];

/*
 * The switch's checks of malformed and hostile frames (dos_switch()), as the
 * settings DOS_<name> name them, each with its bit in dos_checks: the ones that
 * are on or off, then the flood checks, which are off or have a threshold, with
 * the name of the parameter of the threshold.
 */
struct dos_check {
	const char *name;
	int bit;
	const char *param;
};

#define DOS_NONOFF 12
#define DOS_NFLOODS 3
extern const struct dos_check DOS_ONOFF[DOS_NONOFF];
extern const struct dos_check DOS_FLOODS[DOS_NFLOODS];

/* The program that follows as the instance NAME of procd's service "onu". */
#define SVC_START(name, ...) svc_start(name, (const char *const[]){ __VA_ARGS__, NULL })

const char *kv_get(const char *key);
__attribute((format(printf, 1, 2))) const char *kv_getf(const char *fmt, ...);
const char *setting(int n, const char *k);
const char *ipoe_setting(int k, const char *key);
const char *lan_setting(int n, const char *k);
void load(void);
char *session_vlan(int n, char *out);
int sessions(int *out);
bool have_sessions(void);
const char *family(int n);
char *down_vlan(int n, char *out);
char *link_of(const char *vlan, char *out);
bool hands(int n, const char *fam);
int main_pass(void);
int main_pd(void);
bool is_main_route(const char *kind, int n);
int table_of(int n);
char *table_of_conn(const char *conn, char *out);
void pass_links(struct words *out);
void svc_start(const char *name, const char *const *args);
void svc_stop(const char *name);
int ipoes(int *out);
char *ipoe_vlan(int k, char *out);
char *ipoe_dev(int k, char *out);
int ipoe_on_dev(const char *dev);
bool ipoe_carries(int k, const char *fam);
int ipoe_main6(void);
bool in_passthrough(void);
int lans(int *out);
char *lan_if(int n, char *out);
const char *lan_conn(int n);
char *lan_net(int n, char *out);
void lan_conns(struct words *out);
void conf_lines(struct words *lines);
void load_mode(void);
void paths(void);

#endif
