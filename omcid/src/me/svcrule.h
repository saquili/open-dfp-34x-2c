/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com> */

/*
 * The stick's rules for the services the OLT set up, on its one SFP lane
 * to the host. A service is known by its VLAN on the fibre, the network
 * VID of its VLAN rules (class 171). A person's rule moves it to another
 * VLAN towards the host, keeps it off the host, or keeps it to the
 * Ethernet UNIs or to the VEIP alone; and when the rules of two different
 * UNIs or of a UNI and the VEIP would reach the host in one VLAN, the
 * programming moves the later service to a free VLAN by itself
 * (omci_vlan_made()). The MIB stays as the OLT made it: the rules change
 * only what the hardware is given.
 */

#ifndef OMCID_ME_SVCRULE_H
#define OMCID_ME_SVCRULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * enum omci_svc_action - What a rule does with its service.
 * @OMCI_SVC_MOVE: give it the host VLAN of the rule
 * @OMCI_SVC_OFF: keep it off the host
 * @OMCI_SVC_ONLY_UNI: keep the rules of the Ethernet UNIs alone
 * @OMCI_SVC_ONLY_VEIP: keep the rules of the VEIP alone
 */
enum omci_svc_action {
	OMCI_SVC_MOVE = 0,
	OMCI_SVC_OFF = 1,
	OMCI_SVC_ONLY_UNI = 2,
	OMCI_SVC_ONLY_VEIP = 3,
};

/* The most rules: a person's, and the ones a programming makes. */
#define OMCI_SVC_MAX 32u

/**
 * struct omci_svc_rule - A rule for one service.
 * @fibre: the service's VLAN on the fibre, 1 to 4094
 * @host: the VLAN towards the host, of a move
 * @action: enum omci_svc_action
 * @made: made by the programming for a clash, not a person's
 */
struct omci_svc_rule {
	uint16_t fibre;
	uint16_t host;
	uint8_t action;
	bool made;
};

/**
 * omci_svc_parse() - Read the rules of a text, a line each.
 * @text: lines of "FIBRE move HOST", "FIBRE off", "FIBRE only_uni" or
 *	"FIBRE only_veip", the VIDs 1 to 4094 in decimal; NUL-terminated
 * @out: the rules
 * @cap: the places of @out
 *
 * A line that is none of these is left out, as is a second rule for a
 * VID and any beyond @cap.
 *
 * Return: the number of rules.
 */
size_t omci_svc_parse(const char *text, struct omci_svc_rule *out, size_t cap);

/**
 * omci_svc_find() - The rule of a service.
 * @rules: the rules
 * @n: their number
 * @fibre: the service's VLAN on the fibre
 *
 * Return: the rule, or NULL without one.
 */
const struct omci_svc_rule *omci_svc_find(const struct omci_svc_rule *rules,
					  size_t n, uint16_t fibre);

/**
 * omci_svc_action_name() - The word of an action, as a rules text has it.
 * @action: enum omci_svc_action
 *
 * Return: a constant string, "?" for another value.
 */
const char *omci_svc_action_name(uint8_t action);

#endif /* OMCID_ME_SVCRULE_H */
