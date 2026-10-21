// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The PPPoE login the OLT sent, handed from the identity page to the form of
 * a new connection on the WAN page, which takes it once: it is kept in this
 * module alone, never in the address or the browser's storage, and is gone
 * once taken.
 */

import type { OltLogin } from "../api/types";

let pending: OltLogin | null = null;

/** Hand @login to the next form of a new connection. */
export function handOltLogin(login: OltLogin) {
  pending = login;
}

/** The login handed over, once; null when there is none. */
export function takeOltLogin(): OltLogin | null {
  const l = pending;
  pending = null;
  return l;
}
