// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * An update file as the browser reads it before it goes to the stick: the
 * version, the board and the time of its header (onu-utils/src/update.h),
 * the SHA-256 of the whole file, which a release's SHA256SUMS lists, and its
 * signature, checked with the keys the stick trusts. The stick checks all of
 * it again as it writes; this is what a person sees before sending.
 *
 * The signature is a block of SIG_BLOCK bytes right after the SquashFS image,
 * inside the root filesystem's part, which the header's length covers:
 * "ONUS", the format 1, the SHA-256 of the key's raw public key, and the
 * Ed25519 signature of SIG_CONTEXT followed by the SHA-256 of the header,
 * the kernel image and the SquashFS image.
 */

import type { FirmwareBankSignature, FirmwareKeysItem } from "../api/types";

const HEADER = 256;
const TRAILER = 32;
export const SIG_BLOCK = 128;
export const SIG_CONTEXT = "open-dfp-34x-2c update\0";

export interface UpdateFile {
  version: string;
  board: string;
  /** the time of its build, in seconds since 1970 */
  time: number;
  /** the SHA-256 of the whole file, lowercase hex */
  sha256: string;
  /** null: signed, but this browser cannot check Ed25519; the stick will */
  signature: Exclude<FirmwareBankSignature, null> | null;
  signer: string | null;
}

/** Why a file is no update file, in the words of onu-update. */
export class NotUpdate extends Error {}

const hex = (b: ArrayBuffer | Uint8Array) =>
  Array.from(b instanceof Uint8Array ? b : new Uint8Array(b), (x) =>
    x.toString(16).padStart(2, "0"),
  ).join("");

const text = (b: Uint8Array) => new TextDecoder().decode(b).replace(/\0+$/, "");

/* Copy into a buffer of its own, which WebCrypto takes whatever the source. */
const own = (b: Uint8Array) => new Uint8Array(b);

async function sha256(b: Uint8Array): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", own(b)));
}

function fromBase64(s: string): Uint8Array {
  return Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
}

/** Check @sig over @message with the raw public key @raw; null where Ed25519 is missing. */
async function ed25519(raw: Uint8Array, sig: Uint8Array, message: Uint8Array) {
  let key: CryptoKey;
  try {
    key = await crypto.subtle.importKey("raw", own(raw), { name: "Ed25519" }, false, ["verify"]);
  } catch {
    return null;
  }
  return crypto.subtle.verify({ name: "Ed25519" }, key, own(sig), own(message));
}

/** Read @data, a whole update file, as onu-update would, short of the board. */
export async function readUpdate(data: Uint8Array, keys: FirmwareKeysItem[]): Promise<UpdateFile> {
  if (data.length < HEADER + TRAILER) throw new NotUpdate("too short for an update file");
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (text(data.subarray(0, 4)) !== "ONUF" || view.getUint32(8) !== HEADER)
    throw new NotUpdate("not an update file");
  const kernelLen = view.getUint32(12);
  const rootfsLen = view.getUint32(16);
  const body = HEADER + kernelLen + rootfsLen;
  if (data.length !== body + TRAILER)
    throw new NotUpdate("its length does not match its header: a damaged file");
  if (hex(await sha256(data.subarray(0, body))) !== hex(data.subarray(body)))
    throw new NotUpdate("the checksum does not match: the file is damaged");

  const rootfs = HEADER + kernelLen;
  const used = Number(view.getBigUint64(rootfs + 40, true));
  const file: UpdateFile = {
    version: text(data.subarray(96, 160)),
    board: text(data.subarray(32, 96)),
    time: Number(view.getBigUint64(24)),
    sha256: hex(await sha256(data)),
    signature: "none",
    signer: null,
  };
  const block = data.subarray(rootfs + used, body);
  if (block.length !== SIG_BLOCK || text(block.subarray(0, 4)) !== "ONUS") return file;

  const id = hex(block.subarray(8, 40));
  const key = keys.find((k) => k.id === id);
  if (!key) return { ...file, signature: "unknown" };
  const digest = await sha256(data.subarray(0, rootfs + used));
  const message = new Uint8Array([...new TextEncoder().encode(SIG_CONTEXT), ...digest]);
  const ok = await ed25519(fromBase64(key.public), block.subarray(40, 104), message);
  if (ok === null) return { ...file, signature: null, signer: key.name };
  return ok ? { ...file, signature: "valid", signer: key.name } : { ...file, signature: "bad" };
}
