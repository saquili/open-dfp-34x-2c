// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * System's ways into the stick, on three pages: Panel, SSH and HTTPS. Root's
 * password, which the console, ssh and the panel take; the sessions of the
 * panel, this browser's among them, and the end of the others; the token of
 * scripts, made anew or taken away; the ssh server, its host key and whether
 * it takes keys alone, and the keys root logs in with, the panel's and the
 * image's; the panel's HTTPS, the certificate of each address, and an own
 * certificate in place of the stick's. Every change but the password's own
 * asks for root's password at the moment it is made (components/Confirm), and
 * it is never kept. After a new key or certificate of the panel the browser
 * meets a certificate it has not seen, and warns once unless it trusts its
 * authority: the HTTPS card says so and shows the fingerprint to compare, with
 * a button that reloads the page.
 */

import { zodResolver } from "@hookform/resolvers/zod";
import { useNavigate } from "@tanstack/react-router";
import {
  Check,
  CircleAlert,
  Copy,
  KeyRound,
  LogOut,
  RotateCw,
  ShieldCheck,
  Trash2,
  Undo2,
  Users,
} from "lucide-react";
import { type ReactNode, useState } from "react";
import { useForm } from "react-hook-form";
import { useChangePassword, useSignOut } from "../../api/auth";
import {
  useEndOthers,
  useSessions,
  useSsh,
  useSshHostKey,
  useSshKeyAdd,
  useSshKeyRemove,
  useSshKeysOnly,
  useTls,
  useTlsCertificate,
  useTlsKey,
  useTlsSelfSigned,
  useToken,
  useTokenNew,
  useTokenOff,
} from "../../api/config";
import type { Sessions, SshKey, Tls } from "../../api/types";
import { useConfirm } from "../../components/Confirm";
import { Field, SaveState } from "../../components/Field";
import { Page } from "../../components/Page";
import { QueryNote, QueryState } from "../../components/Read";
import { Badge, Card, Cell, Detail, Dot, Facts, Row, Table } from "../../components/ui";
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
import type { z } from "../../lib/schemas";
import { passwordFailure, passwordSchema } from "../SignIn";
import { Columns, Hint, Mono, Pair, Stack } from "./shared";

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

/** A warning in words, after its icon in the state's colour. */
const Warn = ({ children }: { children: ReactNode }) => (
  <p className="flex items-start gap-2 text-sm">
    <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
    <span>{children}</span>
  </p>
);

/* ------------------------------------------------------------- password */

/*
 * Root's password: the console's, ssh's and the panel's. The form asks for
 * the one in force itself, so it needs no confirmation; a new one ends
 * every other session of the panel.
 */
function RootPassword() {
  const { t } = useI18n();
  const change = useChangePassword();
  const schema = passwordSchema(t);
  const form = useForm<z.infer<typeof schema>>({
    resolver: zodResolver(schema),
    defaultValues: { current: "", next: "", repeat: "" },
  });
  const errors = form.formState.errors;
  const failed = passwordFailure(t, change.error);
  return (
    <Card title={t("access.passwordTitle")}>
      <form
        className="grid gap-4"
        noValidate
        onSubmit={form.handleSubmit(({ current, next }) =>
          change.mutate(
            { current, next },
            { onSuccess: () => form.reset({ current: "", next: "", repeat: "" }) },
          ),
        )}
      >
        <Field
          label={t("auth.current")}
          secret
          autoComplete="current-password"
          error={errors.current?.message}
          {...form.register("current")}
        />
        <Field
          label={t("auth.next")}
          secret
          autoComplete="new-password"
          error={errors.next?.message}
          {...form.register("next")}
        />
        <Field
          label={t("auth.repeat")}
          secret
          autoComplete="new-password"
          error={errors.repeat?.message}
          hint={t("access.passwordRules")}
          {...form.register("repeat")}
        />
        <div className="flex flex-wrap items-center gap-3">
          <button type="submit" className="btn btn-primary btn-sm" disabled={change.isPending}>
            <KeyRound className="size-4" />
            {t("auth.save")}
          </button>
          <SaveState
            state={change.isPending ? "busy" : failed ? "bad" : change.isSuccess ? "ok" : "idle"}
            message={
              change.isPending
                ? t("internet.saving")
                : (failed ?? (change.isSuccess ? t("access.passwordChanged") : undefined))
            }
          />
        </div>
      </form>
    </Card>
  );
}

/* ------------------------------------------------------------- sessions */

/** How long ago @s seconds were, in words: "2 min ago". */
function ago(s: number, lang: "pl" | "en") {
  const rel = new Intl.RelativeTimeFormat(lang, { numeric: "always", style: "narrow" });
  if (s < 60) return rel.format(-Math.max(0, Math.round(s)), "second");
  if (s < 3600) return rel.format(-Math.floor(s / 60), "minute");
  return rel.format(-Math.floor(s / 3600), "hour");
}

function SessionRows({ v }: { v: Sessions }) {
  const { t, lang } = useI18n();
  const head = [t("access.sessionOpened"), t("access.sessionSeen"), t("access.sessionFrom")];
  return (
    <Table head={head}>
      {v.sessions.map((s) => (
        <Row key={`${s.since_s}-${s.from}-${s.seen_s}`}>
          <Cell head={head[0]}>
            <span className="flex flex-wrap items-center gap-2">
              {ago(v.now_s - s.since_s, lang)}
              {s.this && <Badge tone="good">{t("access.thisBrowser")}</Badge>}
            </span>
          </Cell>
          <Cell head={head[1]}>{ago(v.now_s - s.seen_s, lang)}</Cell>
          <Cell head={head[2]}>
            <span className="font-mono">{s.from || "–"}</span>
          </Cell>
        </Row>
      ))}
    </Table>
  );
}

/* The panel's sessions: this browser's and the others, which may be ended. */
function SessionsCard() {
  const { t } = useI18n();
  const sessions = useSessions();
  const others = useEndOthers();
  const signOut = useSignOut();
  const navigate = useNavigate();
  const confirm = useConfirm();
  const v = sessions.data;
  const count = v ? v.sessions.filter((s) => !s.this).length : 0;
  return (
    <Card
      title={t("access.sessions")}
      action={
        v && (
          <span className="tabular text-xs text-base-content/70">
            {t("access.count", { n: v.sessions.length, max: v.max })}
          </span>
        )
      }
    >
      <QueryState reads={sessions}>{() => v && <SessionRows v={v} />}</QueryState>
      <Hint>
        {t("access.sessionsHint", {
          max: v?.max ?? 3,
          idle: Math.round((v?.idle_timeout_s ?? 900) / 60),
          age: Math.round((v?.max_age_s ?? 43200) / 3600),
        })}
      </Hint>
      <div className="flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="btn btn-sm btn-outline"
          disabled={!count}
          title={count ? undefined : t("access.noOthers")}
          onClick={() =>
            confirm.open({
              title: t("access.endOthers"),
              body: t("access.endOthersConfirm", { n: count }),
              action: t("access.endOthersAction"),
              run: (secret) => others.mutateAsync({ secret }),
            })
          }
        >
          <Users className="size-4" />
          {t("access.endOthers")}
        </button>
        <button
          type="button"
          className="btn btn-sm btn-ghost"
          disabled={signOut.isPending}
          onClick={() => signOut.mutate(undefined, { onSettled: () => navigate({ to: "/login" }) })}
        >
          <LogOut className="size-4" />
          {t("nav.signOut")}
        </button>
      </div>
      {others.isSuccess && (
        <SaveState state="ok" message={t("access.ended", { n: others.data?.ended ?? 0 })} />
      )}
      {confirm.dialog}
    </Card>
  );
}

/* ---------------------------------------------------------------- token */

/* The token a new one's answer carried: shown here once, with a copy, until hidden. */
function NewToken({ token, onHide }: { token: string; onHide: () => void }) {
  const { t } = useI18n();
  const [copied, setCopied] = useState(false);
  return (
    <div role="status" className="grid gap-2 rounded-box border border-base-300 p-3">
      <p className="flex items-start gap-2 text-sm">
        <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
        {t("access.tokenOnce")}
      </p>
      <code className="break-all rounded-field bg-base-200 px-2 py-1.5 font-mono text-xs">
        {token}
      </code>
      <div className="flex flex-wrap items-center gap-2">
        <button
          type="button"
          className="btn btn-sm"
          onClick={async () => {
            try {
              await navigator.clipboard.writeText(token);
              setCopied(true);
            } catch {
              setCopied(false);
            }
          }}
        >
          {copied ? <Check className="size-4 text-success" /> : <Copy className="size-4" />}
          {t(copied ? "access.copied" : "access.copy")}
        </button>
        <button type="button" className="btn btn-sm btn-ghost" onClick={onHide}>
          {t("access.hide")}
        </button>
      </div>
      <Hint>{t("access.tokenUse")}</Hint>
    </div>
  );
}

/* The token of scripts: whether one is in force, a new one, none. */
function TokenCard() {
  const { t } = useI18n();
  const token = useToken();
  const made = useTokenNew();
  const off = useTokenOff();
  const confirm = useConfirm();
  const set = token.data?.set;
  return (
    <Card title={t("access.token")}>
      <QueryState reads={token}>
        {() => (
          <Facts
            rows={[
              [
                t("access.tokenState"),
                <Badge key="token" tone="neutral">
                  {t(set ? "access.tokenSet" : "access.tokenNone")}
                </Badge>,
              ],
            ]}
          />
        )}
      </QueryState>
      <Hint>{t("access.tokenHint")}</Hint>
      <div className="flex flex-wrap items-center gap-2">
        <button
          type="button"
          className={`btn btn-sm ${set ? "btn-outline" : "btn-primary"}`}
          disabled={!token.data}
          onClick={() => {
            off.reset();
            confirm.open({
              title: t(set ? "access.tokenReplace" : "access.tokenNew"),
              body: t(set ? "access.tokenReplaceConfirm" : "access.tokenNewConfirm"),
              action: t(set ? "access.tokenReplaceAction" : "access.tokenNewAction"),
              run: (secret) => made.mutateAsync({ secret }),
            });
          }}
        >
          <KeyRound className="size-4" />
          {t(set ? "access.tokenReplace" : "access.tokenNew")}
        </button>
        {set && (
          <button
            type="button"
            className="btn btn-sm btn-ghost hover:text-error"
            onClick={() => {
              made.reset();
              confirm.open({
                title: t("access.tokenOff"),
                body: t("access.tokenOffConfirm"),
                action: t("access.tokenOffAction"),
                danger: true,
                run: (secret) => off.mutateAsync({ secret }),
              });
            }}
          >
            <Trash2 className="size-4" />
            {t("access.tokenOff")}
          </button>
        )}
      </div>
      {made.isSuccess &&
        (made.data?.token ? (
          <NewToken token={made.data.token} onHide={() => made.reset()} />
        ) : (
          <p className="text-sm text-error">{t("access.tokenUnseen")}</p>
        ))}
      {off.isSuccess && <SaveState state="ok" message={t("access.tokenGone")} />}
      {confirm.dialog}
    </Card>
  );
}

/* ------------------------------------------------------------------ ssh */

/** The panel's keys, which it may change, apart from the image's. */
const panelKeys = (keys: SshKey[]) => keys.filter((k) => k.source !== "image");

function SshServer() {
  const { t } = useI18n();
  const ssh = useSsh();
  const hostKey = useSshHostKey();
  const only = useSshKeysOnly();
  const confirm = useConfirm();
  const s = ssh.data;
  const noKey = !s || panelKeys(s.keys).length === 0;
  return (
    <Card title={t("access.ssh")}>
      <QueryState reads={ssh}>
        {() => (
          <>
            <Facts
              rows={[
                [
                  t("access.login"),
                  <Badge key="login" tone="neutral">
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
            <Hint>{t("access.hostKeyHint")}</Hint>
            <label className="label cursor-pointer justify-start gap-3 whitespace-normal">
              <input
                type="checkbox"
                className="toggle toggle-sm"
                checked={!!s?.keys_only}
                disabled={noKey && !s?.keys_only}
                onChange={(e) => {
                  const on = e.currentTarget.checked;
                  /* The switch shows the server's state until the change is done. */
                  e.currentTarget.checked = !on;
                  confirm.open({
                    title: t(on ? "access.keysOnlyOn" : "access.keysOnlyOff"),
                    body: t(on ? "access.keysOnlyOnConfirm" : "access.keysOnlyOffConfirm"),
                    action: t(on ? "access.keysOnlyOnAction" : "access.keysOnlyOffAction"),
                    run: (secret) => only.mutateAsync({ on, secret }),
                  });
                }}
              />
              <span>
                <span className="text-sm">{t("access.keysOnlyToggle")}</span>
                <span className="block text-xs text-base-content/70">
                  {t(noKey && !s?.keys_only ? "access.keysOnlyNeedsKey" : "access.keysOnlyHint")}
                </span>
              </span>
            </label>
            <div className="flex flex-wrap items-center gap-3">
              <button
                type="button"
                className="btn btn-sm"
                onClick={() =>
                  confirm.open({
                    title: t("access.hostKeyNew"),
                    body: t("access.hostKeyConfirm"),
                    action: t("access.hostKeyAction"),
                    run: (secret) => hostKey.mutateAsync({ secret }),
                  })
                }
              >
                <RotateCw className="size-4" />
                {t("access.hostKeyNew")}
              </button>
              {hostKey.isSuccess && <SaveState state="ok" message={t("access.hostKeyDone")} />}
            </div>
          </>
        )}
      </QueryState>
      {confirm.dialog}
    </Card>
  );
}

function KeyRow({
  k,
  last,
  onRemove,
}: {
  k: SshKey;
  last: boolean;
  onRemove: (k: SshKey) => void;
}) {
  const { t } = useI18n();
  const image = k.source === "image";
  const known = k.type !== "unknown";
  const head = [
    t("access.keyKind"),
    t("access.keyBits"),
    t("access.keyFingerprint"),
    t("access.keyCommentHead"),
    t("access.keySource"),
  ];
  return (
    <Row>
      <Cell head={head[0]}>
        <span className={known ? "whitespace-nowrap" : ""}>
          {known ? keyKind(k.type) : t("access.keyUnknown")}
        </span>
      </Cell>
      <Cell head={head[1]}>
        <span className="tabular whitespace-nowrap">{k.bits || "–"}</span>
      </Cell>
      <Cell head={head[2]}>{known ? <Mono>{k.fingerprint}</Mono> : "–"}</Cell>
      <Cell head={head[3]}>
        <span className="break-words">{k.comment || "–"}</span>
      </Cell>
      <Cell head={head[4]}>
        <Badge tone="neutral">{t(image ? "access.fromImage" : "access.fromPanel")}</Badge>
      </Cell>
      <Cell actions>
        {image ? (
          <span className="sr-only">{t("access.imageKeyHint")}</span>
        ) : (
          <button
            type="button"
            className="btn btn-ghost btn-sm btn-square text-base-content/70 hover:text-error"
            disabled={last}
            title={last ? t("access.lastKey") : t("access.remove")}
            aria-label={t("access.remove")}
            onClick={() => onRemove(k)}
          >
            <Trash2 className="size-4" />
          </button>
        )}
      </Cell>
    </Row>
  );
}

/* The keys root logs in to ssh with: the panel's, and the image's, read-only. */
function SshKeys() {
  const { t } = useI18n();
  const ssh = useSsh();
  const add = useSshKeyAdd();
  const remove = useSshKeyRemove();
  const confirm = useConfirm();
  const [line, setLine] = useState("");
  const [problem, setProblem] = useState<KeyProblem | null>(null);
  const s = ssh.data;
  const mine = s ? panelKeys(s.keys) : [];
  const image = s ? s.keys.length - mine.length : 0;
  const full = !!s && mine.length >= s.keys_max;
  const head = [
    t("access.keyKind"),
    t("access.keyBits"),
    t("access.keyFingerprint"),
    t("access.keyCommentHead"),
    t("access.keySource"),
    "",
  ];
  const submit = () => {
    const c = checkKey(line);
    setProblem(c.ok ? null : c.problem);
    if (!c.ok) return;
    add.reset();
    remove.reset();
    confirm.open({
      title: t("access.add"),
      body: t("access.addConfirm", {
        kind: keyKind(c.type, c.bits),
        comment: c.comment || "–",
      }),
      action: t("access.addAction"),
      run: async (secret) => {
        await add.mutateAsync({ key: line, secret });
        setLine("");
      },
    });
  };
  return (
    <Card
      title={t("access.keys")}
      action={
        s && (
          <span className="tabular text-xs text-base-content/70">
            {t("access.keysCount", { n: mine.length, max: s.keys_max, image })}
          </span>
        )
      }
    >
      <QueryNote reads={ssh} />
      {s && s.keys.length === 0 && <p className="text-sm">{t("access.keysEmpty")}</p>}
      {s && s.keys.length > 0 && (
        <Table head={head}>
          {s.keys.map((k) => (
            <KeyRow
              key={`${k.source}-${k.fingerprint}-${k.comment}`}
              k={k}
              last={s.keys_only && mine.length === 1}
              onRemove={(key) => {
                add.reset();
                remove.reset();
                confirm.open({
                  title: t("access.remove"),
                  body: t("access.removeConfirm", { fingerprint: key.fingerprint }),
                  action: t("access.removeAction"),
                  danger: true,
                  run: (secret) => remove.mutateAsync({ fingerprint: key.fingerprint, secret }),
                });
              }}
            />
          ))}
        </Table>
      )}
      {image > 0 && <Hint>{t("access.imageKeyHint")}</Hint>}
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
            <Hint>
              {t(full ? "access.full" : "access.publicKeyHint", { max: s?.keys_max ?? 8 })}
            </Hint>
          )}
        </div>
        <div className="flex flex-wrap items-center gap-3">
          <button type="submit" className="btn btn-primary btn-sm" disabled={!line.trim() || full}>
            <KeyRound className="size-4" />
            {t("access.add")}
          </button>
          {add.isSuccess && <SaveState state="ok" message={t("access.added")} />}
          {remove.isSuccess && <SaveState state="ok" message={t("access.removed")} />}
        </div>
      </form>
      {confirm.dialog}
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
      <Warn>{t("access.changed")}</Warn>
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

/* The panel's HTTPS: this connection, the certificate of each address, a new key. */
function Https({ changed, onChanged }: { changed: boolean; onChanged: () => void }) {
  const { t, lang } = useI18n();
  const tls = useTls();
  const newKey = useTlsKey();
  const confirm = useConfirm();
  const here = window.location.hostname;
  const secure = window.location.protocol === "https:";
  const v = tls.data;
  const warnings = certWarnings(v, here, Date.now());
  const head = [
    t("access.address"),
    t("access.issuedBy"),
    t("access.fingerprint"),
    t("access.validUntil"),
    t("access.namesIt"),
  ];
  return (
    <Card title={t("access.https")}>
      <Facts
        rows={[
          [
            t("system.connection"),
            <span key="tls" className="flex items-center gap-2">
              <Dot tone={secure ? "good" : "warn"} />
              {t(secure ? "system.connectionHttps" : "system.connectionHttp")}
            </span>,
          ],
        ]}
      />
      <QueryState reads={tls}>
        {() => (
          <>
            {v && v.certificates.length > 0 ? (
              <Table head={head}>
                {v.certificates.map((c) => (
                  <Row key={`${c.address}:${c.port}`}>
                    <Cell head={head[0]}>
                      <span className="font-mono">{c.address}</span>
                      {c.port !== 443 && <Detail>{t("access.port", { port: c.port })}</Detail>}
                      {c.address === here && <Detail>{t("access.openHere")}</Detail>}
                    </Cell>
                    <Cell head={head[1]}>{t(v.own ? "access.own" : "access.selfSigned")}</Cell>
                    <Cell head={head[2]}>
                      <Mono>{c.sha256}</Mono>
                    </Cell>
                    <Cell head={head[3]}>{day(Date.parse(c.not_after) / 1000, lang)}</Cell>
                    <Cell head={head[4]}>
                      <span className="flex items-center gap-2">
                        <Dot tone={c.named ? "good" : "warn"} />
                        {t(c.named ? "access.yes" : "access.no")}
                      </span>
                    </Cell>
                  </Row>
                ))}
              </Table>
            ) : (
              <p className="text-sm">{t("access.noHttps")}</p>
            )}
            {warnings.map((w) => (
              <Warn key={w}>
                {t(WARNING[w], {
                  host: here,
                  date: v?.own ? day(Date.parse(v.own.not_after) / 1000, lang) : "",
                })}
              </Warn>
            ))}
            {!v?.own && v && v.certificates.length > 0 && (
              <Hint>{t("system.certificateHint")}</Hint>
            )}
            {changed && <Reload tls={v} />}
            <div className="flex flex-wrap items-center gap-3">
              <button
                type="button"
                className="btn btn-sm"
                onClick={() =>
                  confirm.open({
                    title: t("access.newKey"),
                    body: t(v?.own ? "access.newKeyOwnConfirm" : "access.newKeyConfirm"),
                    action: t("access.newKeyAction"),
                    run: async (secret) => {
                      await newKey.mutateAsync({ secret });
                      if (!v?.own) onChanged();
                    },
                  })
                }
              >
                <RotateCw className="size-4" />
                {t("access.newKey")}
              </button>
              {newKey.isSuccess && v?.own && (
                <SaveState state="ok" message={t("access.newKeyWaits")} />
              )}
            </div>
          </>
        )}
      </QueryState>
      {confirm.dialog}
    </Card>
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
              const input = e.currentTarget;
              const file = input.files?.[0];
              if (file) onChange(await file.text());
              input.value = "";
            }}
          />
        </label>
      </div>
      <textarea
        id={id}
        className={`textarea w-full font-mono text-xs ${invalid ? "textarea-error" : ""}`}
        rows={4}
        spellcheck={false}
        value={value}
        onInput={(e) => onChange((e.currentTarget as HTMLTextAreaElement).value)}
      />
    </div>
  );
}

/* An own certificate: what it is, when there is one, and the form that sets one. */
function OwnCertificate({ onChanged }: { onChanged: () => void }) {
  const { t, lang } = useI18n();
  const tls = useTls();
  const own = useTlsCertificate();
  const back = useTlsSelfSigned();
  const confirm = useConfirm();
  const [certificate, setCertificate] = useState("");
  const [key, setKey] = useState("");
  const [problem, setProblem] = useState<PemProblem | null>(null);
  const o = tls.data?.own;
  const send = () => {
    const p = checkPem(certificate, key);
    setProblem(p);
    if (p) return;
    confirm.open({
      title: t("access.use"),
      body: t("access.useConfirm"),
      action: t("access.useAction"),
      run: async (secret) => {
        await own.mutateAsync({ certificate, key, secret });
        setCertificate("");
        setKey("");
        onChanged();
      },
    });
  };
  return (
    <Card title={t("access.ownTitle")}>
      <QueryState reads={tls}>
        {() =>
          o ? (
            <>
              <Facts
                rows={[
                  [t("access.subject"), o.subject],
                  [t("access.issuer"), o.issuer],
                  [t("access.chain"), o.chain],
                  [
                    t("access.valid"),
                    t("access.validSpan", {
                      from: day(Date.parse(o.not_before) / 1000, lang),
                      until: day(Date.parse(o.not_after) / 1000, lang),
                    }),
                  ],
                ]}
              />
              <Pair label={t("access.names")}>
                {o.names.length ? o.names.map((n) => <Mono key={n}>{n}</Mono>) : "–"}
              </Pair>
              <div>
                <button
                  type="button"
                  className="btn btn-sm btn-outline"
                  onClick={() =>
                    confirm.open({
                      title: t("access.back"),
                      body: t("access.backConfirm"),
                      action: t("access.backAction"),
                      run: async (secret) => {
                        await back.mutateAsync({ secret });
                        onChanged();
                      },
                    })
                  }
                >
                  <Undo2 className="size-4" />
                  {t("access.back")}
                </button>
              </div>
            </>
          ) : (
            <p className="text-sm">{t("access.ownNone")}</p>
          )
        }
      </QueryState>
      <form
        className="grid gap-3 border-t border-base-300 pt-4"
        noValidate
        onSubmit={(e) => {
          e.preventDefault();
          send();
        }}
      >
        <div className="grid gap-1">
          <h3 className="text-sm font-semibold">{t(o ? "access.ownReplace" : "access.ownSet")}</h3>
          <Hint>{t("access.ownHint")}</Hint>
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
            disabled={!certificate.trim() || !key.trim()}
          >
            <ShieldCheck className="size-4" />
            {t("access.use")}
          </button>
          {own.isSuccess && <SaveState state="ok" message={t("access.ownTaken")} />}
          {back.isSuccess && <SaveState state="ok" message={t("access.backDone")} />}
        </div>
      </form>
      {confirm.dialog}
    </Card>
  );
}

/*
 * The ways in, a page each, each on one screen: two columns of cards that
 * keep their own heights.
 */
export function PanelAccessPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.panelAccess")} hint={t("access.panelHint")}>
      <Columns>
        <RootPassword />
        <Stack>
          <SessionsCard />
          <TokenCard />
        </Stack>
      </Columns>
    </Page>
  );
}

export function SshPage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.ssh")} hint={t("access.sshHint")}>
      <Columns wide="right">
        <SshServer />
        <SshKeys />
      </Columns>
    </Page>
  );
}

export function HttpsPage() {
  const { t } = useI18n();
  /* A new certificate is served: the HTTPS card asks for a reload. */
  const [changed, setChanged] = useState(false);
  const onChanged = () => setChanged(true);
  return (
    <Page title={t("nav.https")} hint={t("access.httpsHint")}>
      <Columns wide="left">
        <Https changed={changed} onChanged={onChanged} />
        <OwnCertificate onChanged={onChanged} />
      </Columns>
    </Page>
  );
}
