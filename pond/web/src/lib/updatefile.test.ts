// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The update file as the browser reads it, on files built here in the layout
 * of onu-utils/src/update.h and signed with WebCrypto's Ed25519; the tests of
 * onu-update and of signupdate.py check the same layout with Monocypher and
 * OpenSSL.
 */

import { describe, expect, it } from "vitest";
import type { FirmwareKeysItem } from "../api/types";
import { NotUpdate, readUpdate, SIG_BLOCK, SIG_CONTEXT } from "./updatefile";

const enc = new TextEncoder();
const KERNEL_TIME = 1792628168;

const sha256 = async (b: Uint8Array) =>
  new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(b)));
const hex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, "0")).join("");
const cat = (...parts: Uint8Array[]) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
};

function header(kernelLen: number, rootfsLen: number) {
  const h = new Uint8Array(256);
  const v = new DataView(h.buffer);
  h.set(enc.encode("ONUF"));
  v.setUint32(4, 1);
  v.setUint32(8, 256);
  v.setUint32(12, kernelLen);
  v.setUint32(16, rootfsLen);
  v.setBigUint64(24, BigInt(KERNEL_TIME));
  h.set(enc.encode("odi,dfp-34x-2c3"), 32);
  h.set(enc.encode("2026.10.22-0016"), 96);
  return h; /* the CRC is the stick's to check; the browser does not */
}

const kernel = new Uint8Array(512).fill(7);
const squash = (() => {
  const s = new Uint8Array(3000).fill(9);
  s.set(enc.encode("hsqs"));
  new DataView(s.buffer).setBigUint64(40, BigInt(s.length), true);
  return s;
})();

async function key(name: string) {
  const pair = (await crypto.subtle.generateKey({ name: "Ed25519" }, true, [
    "sign",
    "verify",
  ])) as CryptoKeyPair;
  const raw = new Uint8Array(await crypto.subtle.exportKey("raw", pair.publicKey));
  const item: FirmwareKeysItem = {
    name,
    id: hex(await sha256(raw)),
    public: btoa(String.fromCharCode(...raw)),
  };
  return { pair, raw, item };
}

/* An update file, signed with @signer unless null. */
async function file(signer: CryptoKeyPair | null, raw?: Uint8Array) {
  if (!signer || !raw) {
    const body = cat(header(kernel.length, squash.length), kernel, squash);
    return cat(body, await sha256(body));
  }
  const head = header(kernel.length, squash.length + SIG_BLOCK);
  const digest = await sha256(cat(head, kernel, squash));
  const sig = new Uint8Array(
    await crypto.subtle.sign(
      { name: "Ed25519" },
      signer.privateKey,
      cat(enc.encode(SIG_CONTEXT), digest),
    ),
  );
  const block = new Uint8Array(SIG_BLOCK);
  block.set(enc.encode("ONUS"));
  new DataView(block.buffer).setUint32(4, 1);
  block.set(await sha256(raw), 8);
  block.set(sig, 40);
  const body = cat(head, kernel, squash, block);
  return cat(body, await sha256(body));
}

/* @data changed at @at, its trailer made anew, so that only the signature tells. */
async function changed(data: Uint8Array, at: number) {
  const body = data.slice(0, -32);
  body[at] = (body[at] ?? 0) ^ 1;
  return cat(body, await sha256(body));
}

describe("readUpdate", () => {
  it("reads the header and the SHA-256 of the whole file", async () => {
    const data = await file(null);
    const u = await readUpdate(data, []);
    expect(u.version).toBe("2026.10.22-0016");
    expect(u.board).toBe("odi,dfp-34x-2c3");
    expect(u.time).toBe(KERNEL_TIME);
    expect(u.sha256).toBe(hex(await sha256(data)));
    expect(u.signature).toBe("none");
  });

  it("checks a signature with the key that the stick trusts", async () => {
    const k = await key("releases, key 1");
    const u = await readUpdate(await file(k.pair, k.raw), [k.item]);
    expect([u.signature, u.signer]).toEqual(["valid", "releases, key 1"]);
  });

  it("calls a key the stick does not trust unknown", async () => {
    const k = await key("own builds");
    const trusted = await key("releases, key 1");
    const u = await readUpdate(await file(k.pair, k.raw), [trusted.item]);
    expect([u.signature, u.signer]).toEqual(["unknown", null]);
  });

  it("calls a file changed after its signature bad", async () => {
    const k = await key("releases, key 1");
    const data = await changed(await file(k.pair, k.raw), 256 + 512 + 1000);
    const u = await readUpdate(data, [k.item]);
    expect(u.signature).toBe("bad");
  });

  it("is no update file when it is not one, or damaged", async () => {
    await expect(readUpdate(new Uint8Array(100), [])).rejects.toThrow(NotUpdate);
    const data = await file(null);
    data[300] = (data[300] ?? 0) ^ 1;
    await expect(readUpdate(data, [])).rejects.toThrow("the checksum does not match");
    await expect(readUpdate((await file(null)).slice(0, -1), [])).rejects.toThrow(
      "its length does not match",
    );
  });
});
