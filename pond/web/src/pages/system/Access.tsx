// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * System, tab Access: the keys of the stick's two servers. The ssh server's
 * host key, the public keys root logs in with and whether it takes them
 * alone; the panel's HTTPS, its certificate, a new key for it and an own
 * certificate in its place. Every change here needs root's password again,
 * which the field at the top takes once for all of them and the stick checks
 * with each; it is sent with the change and not kept. After a new key or
 * certificate of the panel the browser meets a certificate it has not seen,
 * and warns once unless it trusts its authority: the card says so and shows
 * the fingerprint to compare, with a button that reloads the page.
 */

import { CircleAlert, KeyRound, RotateCw, ShieldCheck, Trash2, Undo2 } from "lucide-react";
import { type ReactNode, useState } from "react";
import { OfflineError } from "../../api/client";
import {
  useSsh,
  useSshHostKey,
  useSshKeyAdd,
  useSshKeyRemove,
  useSshKeysOnly,
  useTls,
  useTlsCertificate,
  useTlsKey,
  useTlsSelfSigned,
} from "../../api/config";
import type { SshKey, Tls } from "../../api/types";
import { SaveState, SecretInput } from "../../components/Field";
import { QueryNote, QueryState } from "../../components/Read";
import { Badge, Card, Cell, Facts, Row, Table } from "../../components/ui";
import { type Key, useI18n } from "../../i18n";
import { day } from "../../lib/format";
import {
  type CertWarning,
  certificateOf,
  certWarnings,
  checkKey,
  checkPem,
  type KeyProblem,
  keyKind,
  type PemProblem,
} from "../../lib/keys";
import { reason } from "../../lib/read";

const KEY_PROBLEM: Record<KeyProblem, Key> = {
  empty: "access.keyEmpty",
  lines: "access.keyLines",
  options: "access.keyOptions",
  type: "access.keyType",
  base64: "access.keyBase64",
  inside: "access.keyInside",
  rsa: "access.keyRsa",
  comment: "access.keyComment",
};

const PEM_PROBLEM: Record<PemProblem, Key> = {
  noCertificate: "access.pemNoCertificate",
  noKey: "access.pemNoKey",
  encrypted: "access.pemEncrypted",
  rsa: "access.pemRsa",
};

const WARNING: Record<CertWarning, Key> = {
  expired: "access.certExpired",
  soon: "access.certSoon",
  notYet: "access.certNotYet",
  unnamed: "access.certUnnamed",
};

/** A change's state line: under way, done with @done, or why it failed. */
function useOutcome(
  change: { isPending: boolean; isSuccess: boolean; error: Error | null },
  done?: string,
): { state: "idle" | "busy" | "ok" | "bad"; message?: string } {
  const { t } = useI18n();
  if (change.isPending) return { state: "busy", message: t("internet.saving") };
  if (change.error)
    return {
      state: "bad",
      message:
        change.error instanceof OfflineError
          ? t("connection.offline")
          : t("internet.failed", { reason: reason(t, change.error) }),
    };
  if (change.isSuccess && done) return { state: "ok", message: done };
  return { state: "idle" };
}

/** A fingerprint, read character by character. */
const Mono = ({ children }: { children: ReactNode }) => (
  <span className="break-all font-mono text-xs">{children}</span>
);

/** A value too long for a row of facts: its label above it, it under the label. */
function Pair({ label, children }: { label: string; children: ReactNode }) {
  return (
    <div className="grid gap-1">
      <span className="text-xs text-base-content/70">{label}</span>
      <div className="grid gap-1 text-sm">{children}</div>
    </div>
  );
}

function Password({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const { t } = useI18n();
  return (
    <Card title={t("access.passwordTitle")}>
      <div className="grid max-w-md gap-1">
        <label htmlFor="access-password" className="text-sm font-semibold">
          {t("system.password")}
        </label>
        <SecretInput
          id="access-password"
          small
          autoComplete="current-password"
          value={value}
          onInput={(e) => onChange((e.currentTarget as HTMLInputElement).value)}
        />
        <p className="text-xs text-base-content/70">{t("access.passwordHint")}</p>
      </div>
    </Card>
  );
}

/* ------------------------------------------------------------------ ssh */

function SshServer({ secret }: { secret: string }) {
  const { t } = useI18n();
  const ssh = useSsh();
  const hostKey = useSshHostKey();
  const only = useSshKeysOnly();
  const s = ssh.data;
  const keyState = useOutcome(hostKey, t("access.hostKeyDone"));
  const onlyState = useOutcome(only, t("internet.saved"));
  const noKey = !s || s.keys.length === 0;
  return (
    <Card title={t("access.ssh")}>
      <QueryState reads={ssh}>
        {() => (
          <>
            <Facts
              rows={[
                [
                  t("access.login"),
                  <Badge key="login" tone={s?.keys_only ? "good" : "neutral"}>
                    {t(s?.keys_only ? "access.keysOnly" : "access.passwordOrKey")}
                  </Badge>,
                ],
              ]}
            />
            <Pair label={t("access.hostKey")}>
              {s?.host_key ? (
                <>
                  <span>{keyKind(s.host_key.type)}</span>
                  <Mono>{s.host_key.fingerprint}</Mono>
                </>
              ) : (
                t("access.hostKeyNone")
              )}
            </Pair>
            <label className="label cursor-pointer justify-start gap-3 whitespace-normal">
              <input
                type="checkbox"
                className="toggle toggle-sm"
                checked={!!s?.keys_only}
                disabled={!secret || only.isPending || (noKey && !s?.keys_only)}
                onChange={(e) => only.mutate({ on: e.currentTarget.checked, secret })}
              />
              <span>
                <span className="text-sm">{t("access.keysOnlyToggle")}</span>
                <span className="block text-xs text-base-content/70">
                  {t(noKey ? "access.keysOnlyNeedsKey" : "access.keysOnlyHint")}
                </span>
              </span>
            </label>
            <SaveState {...onlyState} />
            <div className="flex flex-wrap items-center gap-3">
              <button
                type="button"
                className="btn btn-sm"
                disabled={!secret || hostKey.isPending}
                onClick={() =>
                  window.confirm(t("access.hostKeyConfirm")) && hostKey.mutate({ secret })
                }
              >
                <RotateCw className="size-4" />
                {t("access.hostKeyNew")}
              </button>
              <SaveState {...keyState} />
            </div>
            <p className="text-xs text-base-content/70">{t("access.hostKeyHint")}</p>
          </>
        )}
      </QueryState>
    </Card>
  );
}

function KeyRow({ k, secret, last }: { k: SshKey; secret: string; last: boolean }) {
  const { t } = useI18n();
  const remove = useSshKeyRemove();
  const state = useOutcome(remove);
  return (
    <Row>
      <Cell head={t("access.key")}>
        <span className="break-words">{k.comment || keyKind(k.type, k.bits)}</span>
        {k.comment && (
          <span className="text-xs text-base-content/70">{keyKind(k.type, k.bits)}</span>
        )}
        <Mono>{k.fingerprint}</Mono>
        {state.state === "bad" && <span className="text-xs text-error">{state.message}</span>}
      </Cell>
      <Cell actions>
        <button
          type="button"
          className="btn btn-ghost btn-sm btn-square text-base-content/70 hover:text-error"
          disabled={!secret || remove.isPending || last}
          title={last ? t("access.lastKey") : t("access.remove")}
          aria-label={t("access.remove")}
          onClick={() =>
            window.confirm(t("access.removeConfirm", { fingerprint: k.fingerprint })) &&
            remove.mutate({ fingerprint: k.fingerprint, secret })
          }
        >
          {remove.isPending ? (
            <span className="loading loading-spinner loading-xs" />
          ) : (
            <Trash2 className="size-4" />
          )}
        </button>
      </Cell>
    </Row>
  );
}

function SshKeys({ secret }: { secret: string }) {
  const { t } = useI18n();
  const ssh = useSsh();
  const add = useSshKeyAdd();
  const [line, setLine] = useState("");
  const [problem, setProblem] = useState<KeyProblem | null>(null);
  const s = ssh.data;
  const full = !!s && s.keys.length >= s.keys_max;
  const state = useOutcome(add, t("access.added"));
  const submit = () => {
    const c = checkKey(line);
    setProblem(c.ok ? null : c.problem);
    if (c.ok) add.mutate({ key: line, secret }, { onSuccess: () => setLine("") });
  };
  return (
    <Card
      title={t("access.keys")}
      action={
        s && (
          <span className="tabular text-xs text-base-content/70">
            {t("access.count", { n: s.keys.length, max: s.keys_max })}
          </span>
        )
      }
    >
      <QueryNote reads={ssh} />
      {s && s.keys.length === 0 && (
        <p className="text-sm text-base-content/70">{t("access.keysEmpty")}</p>
      )}
      {s && s.keys.length > 0 && (
        <Table head={[t("access.key"), ""]}>
          {s.keys.map((k) => (
            <KeyRow
              key={k.fingerprint}
              k={k}
              secret={secret}
              last={s.keys_only && s.keys.length === 1}
            />
          ))}
        </Table>
      )}
      <form
        className="grid gap-3"
        noValidate
        onSubmit={(e) => {
          e.preventDefault();
          submit();
        }}
      >
        <div className="grid gap-1">
          <label htmlFor="access-key" className="text-sm font-semibold">
            {t("access.publicKey")}
          </label>
          <textarea
            id="access-key"
            className={`textarea w-full font-mono text-xs ${problem ? "textarea-error" : ""}`}
            rows={3}
            spellcheck={false}
            placeholder="ssh-ed25519 AAAA… user@host"
            value={line}
            onInput={(e) => {
              setLine((e.currentTarget as HTMLTextAreaElement).value);
              setProblem(null);
            }}
          />
          {problem ? (
            <p className="text-xs text-error">{t(KEY_PROBLEM[problem])}</p>
          ) : (
            <p className="text-xs text-base-content/70">{t("access.publicKeyHint")}</p>
          )}
        </div>
        <div className="flex flex-wrap items-center gap-3">
          <button
            type="submit"
            className="btn btn-primary btn-sm"
            disabled={!secret || !line.trim() || add.isPending || full}
            title={full ? t("access.full", { max: s?.keys_max ?? 8 }) : undefined}
          >
            <KeyRound className="size-4" />
            {t("access.add")}
          </button>
          <SaveState {...state} />
        </div>
      </form>
    </Card>
  );
}

/* ---------------------------------------------------------------- https */

/** What a change of the panel's certificate leaves to do: reload, and compare. */
function Reload({ tls }: { tls: Tls | undefined }) {
  const { t } = useI18n();
  const cert = certificateOf(tls, window.location.hostname);
  if (!cert) return null;
  return (
    <div role="status" className="grid gap-2 rounded-box border border-base-300 p-3 text-sm">
      <p className="flex items-start gap-2">
        <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
        {t("access.changed")}
      </p>
      <Mono>{cert.sha256}</Mono>
      <div>
        <button type="button" className="btn btn-sm" onClick={() => window.location.reload()}>
          <RotateCw className="size-4" />
          {t("access.reload")}
        </button>
      </div>
    </div>
  );
}

/** A text of PEM, typed or pasted, or read from a file. */
function PemField({
  id,
  label,
  value,
  onChange,
  invalid,
}: {
  id: string;
  label: string;
  value: string;
  onChange: (v: string) => void;
  invalid: boolean;
}) {
  const { t } = useI18n();
  return (
    <div className="grid gap-1">
      <div className="flex flex-wrap items-center justify-between gap-2">
        <label htmlFor={id} className="text-sm font-semibold">
          {label}
        </label>
        <label className="btn btn-ghost btn-xs">
          {t("access.chooseFile")}
          <input
            type="file"
            className="hidden"
            accept=".pem,.crt,.cer,.key,text/plain"
            onChange={async (e) => {
              const file = e.currentTarget.files?.[0];
              if (file) onChange(await file.text());
              e.currentTarget.value = "";
            }}
          />
        </label>
      </div>
      <textarea
        id={id}
        className={`textarea w-full font-mono text-xs ${invalid ? "textarea-error" : ""}`}
        rows={5}
        spellcheck={false}
        value={value}
        onInput={(e) => onChange((e.currentTarget as HTMLTextAreaElement).value)}
      />
    </div>
  );
}

function Https({ secret }: { secret: string }) {
  const { t, lang } = useI18n();
  const tls = useTls();
  const newKey = useTlsKey();
  const own = useTlsCertificate();
  const back = useTlsSelfSigned();
  const [certificate, setCertificate] = useState("");
  const [key, setKey] = useState("");
  const [problem, setProblem] = useState<PemProblem | null>(null);
  const here = window.location.hostname;
  const v = tls.data;
  const cert = certificateOf(v, here);
  const warnings = certWarnings(v, here, Date.now());
  const changed = newKey.isSuccess || own.isSuccess || back.isSuccess;
  const keyState = useOutcome(newKey);
  const ownState = useOutcome(own);
  const backState = useOutcome(back);
  const busy = newKey.isPending || own.isPending || back.isPending;
  const send = () => {
    const p = checkPem(certificate, key);
    setProblem(p);
    if (!p)
      own.mutate(
        { certificate, key, secret },
        {
          onSuccess: () => {
            setCertificate("");
            setKey("");
          },
        },
      );
  };
  return (
    <Card title={t("access.https")}>
      <QueryState reads={tls}>
        {() => (
          <>
            <Facts
              rows={[
                [
                  t("access.inUse"),
                  v?.own ? (
                    <Badge key="own" tone="good">
                      {t("access.own")}
                    </Badge>
                  ) : (
                    <Badge key="self" tone="neutral">
                      {t("access.selfSigned")}
                    </Badge>
                  ),
                ],
                cert && [
                  t("system.certificateUntil"),
                  day(Date.parse(cert.not_after) / 1000, lang),
                ],
                v?.own && [t("access.chain"), v.own.chain],
              ]}
            />
            {v?.own && (
              <>
                <Pair label={t("access.subject")}>{v.own.subject}</Pair>
                <Pair label={t("access.issuer")}>{v.own.issuer}</Pair>
                <Pair label={t("access.names")}>
                  {v.own.names.map((n) => (
                    <Mono key={n}>{n}</Mono>
                  ))}
                </Pair>
              </>
            )}
            {cert && (
              <Pair label={t("system.certificate")}>
                <Mono>{cert.sha256}</Mono>
              </Pair>
            )}
            {warnings.map((w) => (
              <p key={w} className="flex items-start gap-2 text-sm">
                <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
                {t(WARNING[w], {
                  host: here,
                  date: v?.own ? day(Date.parse(v.own.not_after) / 1000, lang) : "",
                })}
              </p>
            ))}
            {!v?.own && cert && (
              <p className="text-xs text-base-content/70">{t("system.certificateHint")}</p>
            )}
            {changed && <Reload tls={v} />}
            <div className="flex flex-wrap items-center gap-3">
              <button
                type="button"
                className="btn btn-sm"
                disabled={!secret || busy}
                onClick={() =>
                  window.confirm(t(v?.own ? "access.newKeyOwnConfirm" : "access.newKeyConfirm")) &&
                  newKey.mutate({ secret })
                }
              >
                <RotateCw className="size-4" />
                {t("access.newKey")}
              </button>
              {v?.own && (
                <button
                  type="button"
                  className="btn btn-sm btn-outline"
                  disabled={!secret || busy}
                  onClick={() => window.confirm(t("access.backConfirm")) && back.mutate({ secret })}
                >
                  <Undo2 className="size-4" />
                  {t("access.back")}
                </button>
              )}
            </div>
            <SaveState {...keyState} />
            <SaveState {...backState} />

            <form
              className="grid gap-3 border-t border-base-300 pt-4"
              noValidate
              onSubmit={(e) => {
                e.preventDefault();
                send();
              }}
            >
              <div>
                <h3 className="text-sm font-semibold">{t("access.ownTitle")}</h3>
                <p className="text-xs text-base-content/70">{t("access.ownHint")}</p>
              </div>
              <PemField
                id="access-certificate"
                label={t("access.certificateField")}
                value={certificate}
                onChange={(x) => {
                  setCertificate(x);
                  setProblem(null);
                }}
                invalid={problem === "noCertificate"}
              />
              <PemField
                id="access-key-pem"
                label={t("access.keyField")}
                value={key}
                onChange={(x) => {
                  setKey(x);
                  setProblem(null);
                }}
                invalid={!!problem && problem !== "noCertificate"}
              />
              {problem && <p className="text-xs text-error">{t(PEM_PROBLEM[problem])}</p>}
              <div className="flex flex-wrap items-center gap-3">
                <button
                  type="submit"
                  className="btn btn-primary btn-sm"
                  disabled={!secret || !certificate.trim() || !key.trim() || busy}
                >
                  <ShieldCheck className="size-4" />
                  {t("access.use")}
                </button>
                <SaveState {...ownState} />
              </div>
            </form>
          </>
        )}
      </QueryState>
    </Card>
  );
}

export function AccessTab() {
  const [secret, setSecret] = useState("");
  return (
    <div className="grid min-h-0 flex-1 content-start gap-4 lg:grid-cols-2">
      <div className="lg:col-span-2">
        <Password value={secret} onChange={setSecret} />
      </div>
      {/* A fingerprint in a reason or a name breaks where it must, in its column. */}
      <div className="grid min-w-0 content-start gap-4 wrap-anywhere">
        <SshServer secret={secret} />
        <SshKeys secret={secret} />
      </div>
      <div className="grid min-w-0 content-start wrap-anywhere">
        <Https secret={secret} />
      </div>
    </div>
  );
}
