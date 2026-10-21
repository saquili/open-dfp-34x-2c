// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The keys of the stick's two servers, as the panel shows and checks them:
 * the public keys root logs in to ssh with, and the panel's HTTPS
 * certificate. The stick judges a key and a certificate itself (onu-config
 * ssh-keys, pond's src/cert.c); the panel's check, made before anything is
 * sent, takes and refuses the same keys, so that a key pasted wrong is said
 * under its field at once: the types the stick takes, ed25519, ECDSA of
 * nistp256, nistp384 and nistp521 and RSA of 2048 to 4096 bits, one line
 * as ssh-keygen writes it, with a comment of at most 100 bytes.
 */

import type { Tls } from "../api/types";

/** The most keys the stick keeps for root's login (onu-utils/src/sshkey.h). */
export const SSH_KEYS_MAX = 8;
export const RSA_MIN = 2048;
export const RSA_MAX = 4096;
export const COMMENT_MAX = 100;

const CURVES: Record<string, [curve: string, bits: number, point: number]> = {
  "ecdsa-sha2-nistp256": ["nistp256", 256, 65],
  "ecdsa-sha2-nistp384": ["nistp384", 384, 97],
  "ecdsa-sha2-nistp521": ["nistp521", 521, 133],
};

export type KeyProblem =
  | "empty"
  | "lines"
  | "options"
  | "type"
  | "base64"
  | "inside"
  | "rsa"
  | "comment";

export type KeyCheck =
  | { ok: true; type: string; bits: number; comment: string; blob: Uint8Array }
  | { ok: false; problem: KeyProblem };

/* The strings of an SSH blob, RFC 4251 section 5, one after another. */
function strings(blob: Uint8Array): Uint8Array[] | null {
  const out: Uint8Array[] = [];
  for (let at = 0; at < blob.length; ) {
    if (blob.length - at < 4) return null;
    const n =
      ((blob[at] ?? 0) << 24) |
      ((blob[at + 1] ?? 0) << 16) |
      ((blob[at + 2] ?? 0) << 8) |
      (blob[at + 3] ?? 0);
    if (n < 0 || n > blob.length - at - 4) return null;
    out.push(blob.subarray(at + 4, at + 4 + n));
    at += 4 + n;
  }
  return out;
}

const text = (b: Uint8Array | undefined) => (b ? new TextDecoder().decode(b) : "");

/* A control character, which a comment may not hold. */
const control = (ch: string) => ch.charCodeAt(0) < 0x20 || ch.charCodeAt(0) === 0x7f;

/* The bits of a positive mpint, or 0 for one that is not. */
function mpintBits(b: Uint8Array | undefined): number {
  if (!b || b.length === 0 || (b[0] ?? 0) & 0x80) return 0;
  let v = b;
  if (v[0] === 0) {
    if (v.length === 1 || !((v[1] ?? 0) & 0x80)) return 0;
    v = v.subarray(1);
  }
  return v.length * 8 - Math.clz32(v[0] ?? 0) + 24;
}

function decode(b64: string): Uint8Array | null {
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(b64) || b64.length % 4) return null;
  try {
    return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  } catch {
    return null;
  }
}

/** A public key of the authorized_keys form, as the stick will judge it. */
export function checkKey(line: string): KeyCheck {
  const trimmed = line.replace(/^[ \t]+|[ \t\r\n]+$/g, "");
  if (!trimmed) return { ok: false, problem: "empty" };
  if (/[\r\n]/.test(trimmed)) return { ok: false, problem: "lines" };
  const [type = "", b64 = "", ...rest] = trimmed.split(/[ \t]+/);
  const known = type === "ssh-ed25519" || type === "ssh-rsa" || type in CURVES;
  if (!known) return { ok: false, problem: /[=,]/.test(type) ? "options" : "type" };
  const blob = decode(b64);
  if (!blob || b64.length > 1024) return { ok: false, problem: "base64" };
  const parts = strings(blob);
  if (!parts || text(parts[0]) !== type) return { ok: false, problem: "inside" };
  let bits = 256;
  const curve = CURVES[type];
  if (type === "ssh-ed25519") {
    if (parts.length !== 2 || parts[1]?.length !== 32) return { ok: false, problem: "inside" };
  } else if (curve) {
    if (
      parts.length !== 3 ||
      text(parts[1]) !== curve[0] ||
      parts[2]?.length !== curve[2] ||
      parts[2]?.[0] !== 4
    )
      return { ok: false, problem: "inside" };
    bits = curve[1];
  } else {
    const e = parts[1];
    if (
      parts.length !== 3 ||
      mpintBits(e) < 2 ||
      !((e?.[e.length - 1] ?? 0) & 1) ||
      !mpintBits(parts[2])
    )
      return { ok: false, problem: "inside" };
    bits = mpintBits(parts[2]);
    if (bits < RSA_MIN || bits > RSA_MAX) return { ok: false, problem: "rsa" };
  }
  const comment = trimmed.slice(trimmed.indexOf(b64) + b64.length).replace(/^[ \t]+/, "");
  if (
    rest.length &&
    (new TextEncoder().encode(comment).length > COMMENT_MAX || [...comment].some(control))
  )
    return { ok: false, problem: "comment" };
  return { ok: true, type, bits, comment, blob };
}

/** The SHA-256 fingerprint of a key's blob, as ssh-keygen -l prints it. */
export async function fingerprint(blob: Uint8Array): Promise<string> {
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", blob as BufferSource));
  return `SHA256:${btoa(String.fromCharCode(...digest)).replace(/=+$/, "")}`;
}

/** A key's kind in words: Ed25519, ECDSA P-256, RSA 4096, or RSA when its size is not known. */
export function keyKind(type: string, bits = 0): string {
  const curve = CURVES[type];
  if (type === "ssh-ed25519") return "Ed25519";
  if (curve) return `ECDSA P-${curve[1]}`;
  if (type === "ssh-rsa") return bits ? `RSA ${bits}` : "RSA";
  return type;
}

/** The certificate the panel was opened through: the one of @host, else the first. */
export function certificateOf(tls: Tls | undefined, host: string) {
  return tls?.certificates.find((c) => c.address === host) ?? tls?.certificates[0];
}

export type CertWarning = "expired" | "notYet" | "soon" | "unnamed";

/** How near its end a certificate is said to be. */
export const SOON_DAYS = 30;

/**
 * What the panel warns of in the own certificate, at @now (milliseconds, the
 * browser's clock, which is set where the stick's may not be): its end gone
 * or near, its start to come, and that it does not name the address the
 * panel was opened at, which a browser then refuses for that address.
 */
export function certWarnings(tls: Tls | undefined, host: string, now: number): CertWarning[] {
  const own = tls?.own;
  if (!own) return [];
  const out: CertWarning[] = [];
  const until = Date.parse(own.not_after);
  if (until < now) out.push("expired");
  else if (until - now < SOON_DAYS * 86_400_000) out.push("soon");
  if (Date.parse(own.not_before) > now) out.push("notYet");
  if (certificateOf(tls, host)?.named === false) out.push("unnamed");
  return out;
}

export type PemProblem = "noCertificate" | "noKey" | "encrypted" | "rsa";

/**
 * The panel's check of an own certificate before it is sent: a PEM block of
 * a certificate in the first text, and of an unencrypted key, not one of RSA,
 * in the second. The stick reads and judges the rest.
 */
export function checkPem(certificate: string, key: string): PemProblem | null {
  if (!/-----BEGIN CERTIFICATE-----/.test(certificate)) return "noCertificate";
  if (/-----BEGIN ENCRYPTED PRIVATE KEY-----/.test(key)) return "encrypted";
  if (/-----BEGIN RSA PRIVATE KEY-----/.test(key)) return "rsa";
  if (!/-----BEGIN (EC )?PRIVATE KEY-----/.test(key)) return "noKey";
  return null;
}
