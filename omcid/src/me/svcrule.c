// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* The rules of svcrule.h: read from a text, and found by a service's VID. */

#include <string.h>

#include "me/svcrule.h"
#include "omcid/array_size.h"

/* The words of the actions, by enum omci_svc_action. */
static const char *const g_names[] = { "move", "off", "only_uni", "only_veip" };

const char *omci_svc_action_name(uint8_t action)
{
	return action < ARRAY_SIZE(g_names) ? g_names[action] : "?";
}

/*
 * A VID of 1 to 4094 in decimal at *@p, which moves past it; false for
 * anything else.
 */
static bool vid_at(const char **p, uint16_t *out)
{
	unsigned int v = 0;
	const char *s = *p;

	if (*s < '0' || *s > '9')
		return false;
	while (*s >= '0' && *s <= '9') {
		v = v * 10u + (unsigned int)(*s - '0');
		if (v > 4094u)
			return false;
		s++;
	}
	if (v == 0u)
		return false;
	*out = (uint16_t)v;
	*p = s;
	return true;
}

/* The action named by the word at *@p, which moves past it; false for another word. */
static bool action_at(const char **p, uint8_t *out)
{
	for (size_t a = 0; a < ARRAY_SIZE(g_names); a++) {
		const size_t len = strlen(g_names[a]);

		if (strncmp(*p, g_names[a], len) == 0 &&
		    ((*p)[len] == ' ' || (*p)[len] == '\n' ||
		     (*p)[len] == '\0')) {
			*out = (uint8_t)a;
			*p += len;
			return true;
		}
	}
	return false;
}

/* The rule of the line at @s, up to its end; false for a line that is none. */
static bool rule_of(const char *s, struct omci_svc_rule *out)
{
	struct omci_svc_rule r = { 0 };

	if (!vid_at(&s, &r.fibre) || *s++ != ' ' || !action_at(&s, &r.action))
		return false;
	if (r.action == OMCI_SVC_MOVE && (*s++ != ' ' || !vid_at(&s, &r.host)))
		return false;
	if (*s != '\n' && *s != '\0')
		return false;
	*out = r;
	return true;
}

size_t omci_svc_parse(const char *text, struct omci_svc_rule *out, size_t cap)
{
	size_t n = 0;

	for (const char *s = text; *s != '\0' && n < cap;) {
		struct omci_svc_rule r;

		if (rule_of(s, &r) && omci_svc_find(out, n, r.fibre) == NULL)
			out[n++] = r;
		s = strchr(s, '\n');
		if (s == NULL)
			break;
		s++;
	}
	return n;
}

const struct omci_svc_rule *omci_svc_find(const struct omci_svc_rule *rules,
					  size_t n, uint16_t fibre)
{
	for (size_t i = 0; i < n; i++)
		if (rules[i].fibre == fibre)
			return &rules[i];
	return NULL;
}
