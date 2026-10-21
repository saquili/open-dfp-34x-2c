// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The panel's check of a public key and of an own certificate, made before
 * anything is sent: it takes and refuses what the stick does. The keys are
 * those of onu-utils' tests (tests/test_sshkey.c), made with ssh-keygen,
 * and their fingerprints the ones it printed.
 */

import { describe, expect, it } from "vitest";
import type { Tls } from "../api/types";
import {
  certificateOf,
  certWarnings,
  checkKey,
  checkPem,
  fingerprint,
  keyKind,
  SOON_DAYS,
} from "./keys";

const ED25519 =
  "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIJUZp+xf8C/t3NT+0VYL5nV7MNOQXr21ORnzDVmED9xE test-ed25519@example";
const P256 =
  "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBKi6OUiZeRdSZadzWqI/9qS1pPepAzshYaS90KLBFnpy8aHIQZ8IWWdP4jHEn8D0ylXqGBBWU3mMd/CsU/Ueedg= p256";
const P384 =
  "ecdsa-sha2-nistp384 AAAAE2VjZHNhLXNoYTItbmlzdHAzODQAAAAIbmlzdHAzODQAAABhBOxStJC04/gsEYBf6FTihDE0jj7spzwpf/REZWwVFlgoOMe/nN5Getdm3TVkJ5PsBCE2xv9MS8YFgJLvgEvdPqHF39rYJLWNP3pGv7OPz/I6bKzDfOxCxQStU3FT+GY/jA==";
const P521 =
  "ecdsa-sha2-nistp521 AAAAE2VjZHNhLXNoYTItbmlzdHA1MjEAAAAIbmlzdHA1MjEAAACFBAFbPZuVZWzfnAsWYR4ic9PvRS77fRxeT18foYN2wuISYsZze6tC1qI2RzRuhHhnIJoxRaTLq33mZi1BZ24bxmqrnAEW5YwnZ0tR146Mpa2Fbm2ggukvWzdMGH8mF8lXLjVoB8xXCx1Hs6gnv4HNTjKm+QN9pxUWtKQGfn3YCLQHRFpREQ== p521 key";
const RSA1024 =
  "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC4WKVwESjp9sdRo62oW55S64G3eVqIYUlT+q+xVqp03ARK0Oj2yoFl8Df/VkE02pdFdlRASR0aD+o5EoHaKu21mxYU5mwShl85lftmV7po42nmwn7QZu8aoWAtXdDFtK7yvQrn0XBGn3ealTZSGOEHpR52e7BcjqN/o8CIOiQZhw== rsa1024";
const RSA2048 =
  "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDYS+/3Tm0ccRFLiBDR1aFzi3C4V/uIVx1AgIwmu3DW4d4v+UfpdkJNpFWPCS6zmX6N/yEoSADn0SEdQDzF95oJhVyNGefyJS6pEQ4q+4yVK+bLJL8HhyX2lHG2DFmyXDzGVZsNhK6KuyAa1zp26Dm/HeBOanqiwIx4b/iBnWIsDGCySeo1N/wpkQ17RoQRP5P2oyPYoOjm4BfBFGPVFwW5QT/3Ryw+zxtYRSq/jj1CdzbtkxsO+OhA154fY2PwvID7CC1gAsr/caCo/LOmyE/SCKdtaz30HDOtCUNu9YSmPmY7vNp932HEK2Y+qss1n4dSM1OQatVknaLo+KnE+kAF rsa2048";

async function taken(line: string) {
  const c = checkKey(line);
  if (!c.ok) throw new Error(`refused: ${c.problem}`);
  return { ...c, fingerprint: await fingerprint(c.blob) };
}

const problem = (line: string) => {
  const c = checkKey(line);
  return c.ok ? null : c.problem;
};

describe("a public key for root's ssh login", () => {
  it("is taken of each type the stick takes, with ssh-keygen's fingerprint", async () => {
    const ed = await taken(ED25519);
    expect([ed.type, ed.bits, ed.comment, ed.fingerprint]).toEqual([
      "ssh-ed25519",
      256,
      "test-ed25519@example",
      "SHA256:lxckqci4MwTwEjJFI7q4oX1lApk+VkklJ0/B5U0TTc0",
    ]);
    expect((await taken(P256)).fingerprint).toBe(
      "SHA256:QVzpbdO2XiR+U/GoiobfqifyWwLvSY5QBcJvz5JlvTc",
    );
    const p384 = await taken(`${P384} \r\n`);
    expect([p384.bits, p384.comment]).toEqual([384, ""]);
    expect((await taken(P521)).bits).toBe(521);
    const rsa = await taken(RSA2048);
    expect([rsa.bits, rsa.fingerprint]).toEqual([
      2048,
      "SHA256:FfdmUTng6/uN+QeKtmfkTUlfrbXx+C45gyGUEm8XIQ8",
    ]);
  });

  it("is refused, and why, as the stick refuses it", () => {
    expect(problem("")).toBe("empty");
    expect(problem(" \n")).toBe("empty");
    expect(problem(`${ED25519}\n${P256}`)).toBe("lines");
    expect(problem(`command="true" ${ED25519}`)).toBe("options");
    expect(problem(`no-pty,no-agent-forwarding ${ED25519}`)).toBe("options");
    expect(problem("ssh-dss AAAAB3NzaC1kc3M= x")).toBe("type");
    expect(problem("sk-ssh-ed25519@openssh.com AAAA x")).toBe("type");
    expect(problem("ssh-ed25519 AAA")).toBe("base64");
    expect(problem("ssh-ed25519 AA@A")).toBe("base64");
    expect(problem(`ssh-rsa ${"A".repeat(1028)}`)).toBe("base64");
    expect(problem(`ecdsa-sha2-nistp256 ${ED25519.split(" ")[1]}`)).toBe("inside");
    expect(problem("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAAA== x")).toBe("inside");
    expect(problem(RSA1024)).toBe("rsa");
    expect(problem(`${ED25519.slice(0, 80)} ${"x".repeat(101)}`)).toBe("comment");
    expect(problem(`${ED25519.slice(0, 80)} a\tb`)).toBe("comment");
  });

  it("is named by its kind", () => {
    expect(keyKind("ssh-ed25519", 256)).toBe("Ed25519");
    expect(keyKind("ecdsa-sha2-nistp384", 384)).toBe("ECDSA P-384");
    expect(keyKind("ssh-rsa", 4096)).toBe("RSA 4096");
    expect(keyKind("ssh-rsa")).toBe("RSA");
    expect(keyKind("sk-ssh-ed25519@openssh.com")).toBe("sk-ssh-ed25519@openssh.com");
  });
});

const NOW = Date.parse("2026-10-21T12:00:00Z");

function tls(own: Partial<NonNullable<Tls["own"]>> | null, named = true): Tls {
  return {
    https: true,
    own: own && {
      subject: "CN=stick.example",
      issuer: "CN=Example CA",
      sha256: "AA",
      not_before: "2026-01-01T00:00:00Z",
      not_after: "2027-01-01T00:00:00Z",
      chain: 2,
      names: ["IP:192.0.2.254"],
      ...own,
    },
    certificates: [
      { address: "192.0.2.254", port: 443, sha256: "AA", not_before: "", not_after: "", named },
      {
        address: "198.51.100.2",
        port: 443,
        sha256: "AA",
        not_before: "",
        not_after: "",
        named: false,
      },
    ],
  };
}

describe("an own certificate", () => {
  it("is warned of when it ended, ends soon, has not begun, or does not name the address", () => {
    expect(certWarnings(tls(null), "192.0.2.254", NOW)).toEqual([]);
    expect(certWarnings(undefined, "192.0.2.254", NOW)).toEqual([]);
    expect(certWarnings(tls({}), "192.0.2.254", NOW)).toEqual([]);
    expect(certWarnings(tls({ not_after: "2026-10-01T00:00:00Z" }), "192.0.2.254", NOW)).toEqual([
      "expired",
    ]);
    const soon = new Date(NOW + (SOON_DAYS - 1) * 86_400_000).toISOString();
    expect(certWarnings(tls({ not_after: soon }), "192.0.2.254", NOW)).toEqual(["soon"]);
    expect(certWarnings(tls({ not_before: "2026-11-01T00:00:00Z" }), "192.0.2.254", NOW)).toEqual([
      "notYet",
    ]);
    expect(certWarnings(tls({}), "198.51.100.2", NOW)).toEqual(["unnamed"]);
  });

  it("is the certificate of the address the panel is open at, else the first", () => {
    expect(certificateOf(tls({}), "198.51.100.2")?.address).toBe("198.51.100.2");
    expect(certificateOf(tls({}), "stick.example")?.address).toBe("192.0.2.254");
    expect(certificateOf(undefined, "x")).toBeUndefined();
  });

  it("is a certificate's block and an unencrypted key's, not one of RSA, before it is sent", () => {
    const cert = "-----BEGIN CERTIFICATE-----\nQUFB\n-----END CERTIFICATE-----\n";
    expect(checkPem(cert, "-----BEGIN EC PRIVATE KEY-----\nQUFB\n")).toBeNull();
    expect(checkPem(cert, "-----BEGIN PRIVATE KEY-----\nQUFB\n")).toBeNull();
    expect(checkPem("", "-----BEGIN PRIVATE KEY-----")).toBe("noCertificate");
    expect(checkPem(cert, "")).toBe("noKey");
    expect(checkPem(cert, "-----BEGIN ENCRYPTED PRIVATE KEY-----")).toBe("encrypted");
    expect(checkPem(cert, "-----BEGIN RSA PRIVATE KEY-----")).toBe("rsa");
  });
});
