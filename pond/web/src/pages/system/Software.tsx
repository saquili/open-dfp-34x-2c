// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * System, Software: the two systems of the flash, A and B, with the version,
 * build, SHA-256 and signature of each, which runs and which starts next, the
 * one place of these; a trial start of the other one and the way back to the shipped
 * firmware; the update of the one that does not run, which a start of it on
 * trial may follow at once; the download of the running system and the copy
 * of the whole flash; and how an update goes. What needs root's password
 * asks for it in the confirmation (components/Confirm.tsx), which stays over
 * the page while an upload goes.
 */

import { useQueryClient } from "@tanstack/react-query";
import {
  Check,
  CircleAlert,
  Copy,
  Download,
  HardDrive,
  RotateCw,
  Undo2,
  Upload,
} from "lucide-react";
import { useRef, useState } from "react";
import { ApiError } from "../../api/client";
import {
  downloadFlash,
  refresh,
  uploadFirmware,
  useFirmware,
  useReturnBank,
  useTryBank,
} from "../../api/config";
import type { FirmwareBankSignature } from "../../api/types";
import { useConfirm } from "../../components/Confirm";
import { SaveState } from "../../components/Field";
import { Page } from "../../components/Page";
import { QueryNote, QueryState } from "../../components/Read";
import { Badge, Card, Cell, Detail, Facts, Meter, Row, Table } from "../../components/ui";
import { type Key, useI18n } from "../../i18n";
import {
  type BankTag,
  bankLetter,
  bankViews,
  otherBank,
  overShipped,
  shippedBank,
  updateTarget,
  uploading,
} from "../../lib/firmware";
import { bytes, day, percent } from "../../lib/format";
import type { Tone } from "../../lib/state";
import { NotUpdate, readUpdate, type UpdateFile } from "../../lib/updatefile";
import { Columns, Hint, Mono, Stack } from "./shared";

const TAG: Record<BankTag, [Key, Tone]> = {
  running: ["system.tagRunning", "good"],
  default: ["system.tagDefault", "info"],
  once: ["system.tagOnce", "warn"],
  trial: ["system.tagTrial", "warn"],
  failed: ["system.tagFailed", "bad"],
  empty: ["system.tagEmpty", "neutral"],
  incomplete: ["system.tagIncomplete", "bad"],
  shipped: ["system.tagShipped", "warn"],
};

const SIGNATURE: Record<Exclude<FirmwareBankSignature, null>, [Key, Tone]> = {
  valid: ["system.sigValid", "good"],
  none: ["system.sigNone", "warn"],
  unknown: ["system.sigUnknown", "warn"],
  bad: ["system.sigBad", "bad"],
};

/* A signature as a pill, and the name of its key under a valid one. */
function SignatureBadge({ sig, signer }: { sig: FirmwareBankSignature; signer: string | null }) {
  const { t } = useI18n();
  if (sig === null) return <>–</>;
  return (
    <span className="grid justify-items-start gap-1">
      <Badge tone={SIGNATURE[sig][1]}>{t(SIGNATURE[sig][0])}</Badge>
      {signer && <Detail>{signer}</Detail>}
    </span>
  );
}

/* A SHA-256 whole, to be read or compared, and a button that copies it. */
function Sha({ hex }: { hex: string }) {
  const { t } = useI18n();
  const [copied, setCopied] = useState(false);
  return (
    <span className="flex items-start gap-1">
      <Mono>{hex}</Mono>
      <button
        type="button"
        className="btn btn-ghost btn-xs btn-square shrink-0"
        title={t(copied ? "system.shaCopied" : "system.copySha")}
        aria-label={t("system.copySha")}
        onClick={() =>
          void navigator.clipboard.writeText(hex).then(() => {
            setCopied(true);
            setTimeout(() => setCopied(false), 1500);
          })
        }
      >
        {copied ? <Check className="size-3.5 text-success" /> : <Copy className="size-3.5" />}
      </button>
    </span>
  );
}

/* The two systems, what each holds, which runs and which starts next. */
function Systems() {
  const { t, lang } = useI18n();
  const firmware = useFirmware();
  const tryBank = useTryBank();
  const returnBank = useReturnBank();
  const confirm = useConfirm();
  const f = firmware.data;
  const other = f ? otherBank(f) : null;
  const shipped = f ? shippedBank(f) : null;
  const back = bankLetter(f?.running ?? 0);
  const busy = uploading(f) || tryBank.isSuccess || returnBank.isSuccess;
  const head = [
    t("system.system"),
    t("system.version"),
    t("system.built"),
    t("system.sha256"),
    t("system.signature"),
    t("system.state"),
  ];

  return (
    <Card title={t("system.systems")}>
      <QueryState reads={firmware}>
        {() => (
          <>
            <Table head={head}>
              {(f ? bankViews(f) : []).map((b) => (
                <Row key={b.bank}>
                  <Cell head={head[0]}>
                    <span className="whitespace-nowrap font-medium">
                      {t("system.systemLetter", { letter: b.letter })}
                    </span>
                  </Cell>
                  <Cell head={head[1]}>
                    <span className="tabular">{b.version ?? "–"}</span>
                  </Cell>
                  <Cell head={head[2]}>{b.time != null ? day(b.time, lang) : "–"}</Cell>
                  <Cell head={head[3]}>
                    {b.reading ? (
                      <span className="flex items-center gap-2 text-sm">
                        <span className="loading loading-spinner loading-xs" />
                        {t("system.readingBank")}
                      </span>
                    ) : b.sha256 ? (
                      <Sha hex={b.sha256} />
                    ) : (
                      "–"
                    )}
                  </Cell>
                  <Cell head={head[4]}>
                    {b.reading ? "–" : <SignatureBadge sig={b.signature} signer={b.signer} />}
                  </Cell>
                  <Cell head={head[5]}>
                    {b.tags.length ? (
                      <span className="flex flex-wrap gap-1">
                        {b.tags.map((tag) => (
                          <Badge key={tag} tone={TAG[tag][1]}>
                            {t(TAG[tag][0])}
                          </Badge>
                        ))}
                      </span>
                    ) : (
                      "–"
                    )}
                  </Cell>
                </Row>
              ))}
            </Table>
            <Hint>{t("system.shaHint")}</Hint>
            {f?.trial != null && (
              <p className="flex items-start gap-2 text-sm">
                <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
                {t("system.trialNow", { letter: bankLetter(f.trial) })}
              </p>
            )}
            {f?.failed != null && (
              <p className="text-sm text-error">
                {t("system.failedNote", { letter: bankLetter(f.failed) })}
              </p>
            )}
            {f && updateTarget(f) === null && <Hint>{t("system.fromMemory")}</Hint>}
            {(other !== null || shipped !== null) && (
              <div className="flex flex-wrap gap-2">
                {other !== null && (
                  <button
                    type="button"
                    className="btn btn-sm btn-outline"
                    disabled={busy}
                    onClick={() =>
                      confirm.open({
                        title: t("system.switchTo", { letter: bankLetter(other) }),
                        body: t("system.switchConfirm", { letter: bankLetter(other), back }),
                        action: t("system.switchAction"),
                        run: (secret) => tryBank.mutateAsync({ bank: other, secret }),
                      })
                    }
                  >
                    <RotateCw className="size-4" />
                    {t("system.switchTo", { letter: bankLetter(other) })}
                  </button>
                )}
                {shipped !== null && (
                  <button
                    type="button"
                    className="btn btn-sm btn-outline"
                    disabled={busy}
                    onClick={() =>
                      confirm.open({
                        title: t("system.returnTo", { letter: bankLetter(shipped) }),
                        body: t("system.returnConfirm", { letter: bankLetter(shipped) }),
                        action: t("system.returnAction"),
                        danger: true,
                        run: (secret) => returnBank.mutateAsync({ bank: shipped, secret }),
                      })
                    }
                  >
                    <Undo2 className="size-4 text-warning" />
                    {t("system.returnTo", { letter: bankLetter(shipped) })}
                  </button>
                )}
              </div>
            )}
            {tryBank.isSuccess && tryBank.variables?.bank !== undefined && (
              <SaveState
                state="ok"
                message={t("system.switching", { letter: bankLetter(tryBank.variables.bank) })}
              />
            )}
            {returnBank.isSuccess && <SaveState state="ok" message={t("system.returning")} />}
          </>
        )}
      </QueryState>
      {confirm.dialog}
    </Card>
  );
}

type Phase = "idle" | "sending" | "writing" | "done";

/*
 * The update of the system that does not run: a file is chosen, the
 * confirmation says where it goes and what follows, and takes root's
 * password; the file goes up, the stick writes and checks it, and a start of
 * the new system on trial follows, at once when asked for beforehand or when
 * the person asks; the stick keeps it only when it comes up.
 */
function Update() {
  const { t, lang } = useI18n();
  const qc = useQueryClient();
  const firmware = useFirmware();
  const tryBank = useTryBank();
  const input = useRef<HTMLInputElement>(null);
  const [phase, setPhase] = useState<Phase>("idle");
  const [share, setShare] = useState(0);
  const [version, setVersion] = useState("");
  const [startAfter, setStartAfter] = useState(false);
  const [reading, setReading] = useState<string | null>(null);
  const [refused, setRefused] = useState<string | null>(null);
  const f = firmware.data;
  const target = f ? updateTarget(f) : null;
  const letter = target !== null ? bankLetter(target) : "";
  const back = bankLetter(f?.running ?? 0);
  const going = phase === "sending" || phase === "writing";
  const elsewhere = uploading(f) && !going;
  const confirm = useConfirm(
    phase === "sending" ? (
      <Meter share={share} label={t("system.sending", { percent: percent(share, lang) })} />
    ) : phase === "writing" ? (
      <p className="flex items-center gap-2 text-sm">
        <span className="loading loading-spinner loading-xs" />
        {t("system.writing")}
      </p>
    ) : null,
  );

  /* Send @file with @secret; a cut connection is judged by the stick's own account. */
  async function write(file: File, secret: string, start: boolean) {
    setPhase("sending");
    setShare(0);
    try {
      const written = await uploadFirmware(file, secret, (sent, total) => {
        setShare(total ? sent / total : 0);
        if (total && sent >= total) setPhase("writing");
      });
      setVersion(written);
      setPhase("done");
    } catch (e) {
      const now = (await firmware.refetch()).data?.update;
      if (now?.state === "done") {
        setVersion(now.version ?? "");
        setPhase("done");
      } else {
        setPhase("idle");
        const asPassword = e instanceof ApiError && [403, 423, 429].includes(e.status);
        throw !asPassword && now?.message ? new Error(now.message) : e;
      }
    } finally {
      void refresh(qc, "upload");
    }
    if (start && target !== null) await tryBank.mutateAsync({ bank: target, secret });
  }

  /* What the file is, read here before it goes: version, build, SHA-256, signature. */
  function about(u: UpdateFile) {
    const signer = u.signer ?? "";
    const words =
      u.signature === "valid"
        ? t("system.fileSigned", { signer })
        : u.signature === "none"
          ? t("system.fileUnsigned")
          : u.signature === "unknown"
            ? t("system.fileUnknownKey")
            : t("system.fileSignedLater", { signer });
    return (
      <>
        <Facts
          rows={[
            [
              t("system.fileVersion"),
              <span key="version" className="tabular">
                {u.version}
              </span>,
            ],
            [t("system.fileBuilt"), day(u.time, lang)],
            [t("system.fileSha"), <Mono key="sha">{u.sha256}</Mono>],
            [
              t("system.fileSignature"),
              <SignatureBadge key="signature" sig={u.signature} signer={null} />,
            ],
          ]}
        />
        {u.signature === "valid" ? (
          <p>{words}</p>
        ) : (
          <p className="flex items-start gap-2">
            <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
            {words}
          </p>
        )}
      </>
    );
  }

  async function chosen(file: File) {
    if (target === null) return;
    setRefused(null);
    setReading(file.name);
    let u: UpdateFile;
    try {
      u = await readUpdate(new Uint8Array(await file.arrayBuffer()), f?.keys ?? []);
    } catch (e) {
      const why = e instanceof NotUpdate ? e.message : String(e);
      setRefused(t("system.fileNot", { name: file.name, why }));
      return;
    } finally {
      setReading(null);
    }
    if (u.signature === "bad") {
      setRefused(t("system.fileBad"));
      return;
    }
    const shipped = f ? overShipped(f, target) : false;
    const start = startAfter;
    confirm.open({
      title: t("system.writeTitle", { letter }),
      body: (
        <>
          <p>{t("system.writeFile", { name: file.name, size: bytes(file.size, lang), letter })}</p>
          {about(u)}
          {shipped && (
            <p className="flex items-start gap-2">
              <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
              {t("system.overShippedConfirm", { letter })}
            </p>
          )}
          <p>
            {start
              ? t("system.uploadStartConfirm", { letter, back })
              : t("system.writeThen", { letter })}
          </p>
        </>
      ),
      action: t("system.writeAction"),
      danger: shipped || (u.signature !== "valid" && u.signature !== null),
      run: (secret) => write(file, secret, start),
    });
  }

  return (
    <Card title={t("system.update")}>
      <QueryNote reads={firmware} />
      {target === null ? (
        f && <Hint>{t("system.fromMemory")}</Hint>
      ) : (
        <>
          {f && overShipped(f, target) ? (
            <p className="flex items-start gap-1.5 text-sm">
              <CircleAlert className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
              {t("system.uploadHintShipped", { letter })}
            </p>
          ) : (
            <p className="text-sm">{t("system.uploadHint", { letter })}</p>
          )}
          <label className="label cursor-pointer justify-start gap-3 whitespace-normal">
            <input
              type="checkbox"
              className="toggle toggle-sm"
              checked={startAfter}
              disabled={going}
              onChange={(e) => setStartAfter(e.currentTarget.checked)}
            />
            <span>
              <span className="text-sm">{t("system.startAfter")}</span>
              <span className="block text-xs text-base-content/70">
                {t("system.startAfterHint")}
              </span>
            </span>
          </label>
          <input
            ref={input}
            type="file"
            accept=".bin,application/octet-stream"
            className="hidden"
            onChange={(e) => {
              const file = e.currentTarget.files?.[0];
              e.currentTarget.value = "";
              if (file) void chosen(file);
            }}
          />
          <div>
            <button
              type="button"
              className="btn btn-sm btn-primary"
              disabled={going || elsewhere || tryBank.isSuccess || reading !== null}
              onClick={() => input.current?.click()}
            >
              <Upload className="size-4" />
              {t("system.upload")}
            </button>
          </div>
          {reading !== null && (
            <SaveState state="busy" message={t("system.fileReading", { name: reading })} />
          )}
          {refused && <SaveState state="bad" message={refused} />}
          {elsewhere && <p className="text-sm">{t("system.busy")}</p>}
          {/* The new system is started where both are, under Systems, unless at once. */}
          {phase === "done" && (
            <SaveState
              state="ok"
              message={`${t("system.written", { letter, version })} ${
                tryBank.isSuccess ? "" : t("system.writtenThen", { letter })
              }`.trim()}
            />
          )}
          {tryBank.isSuccess && (
            <SaveState state="ok" message={t("system.switching", { letter })} />
          )}
        </>
      )}
      {confirm.dialog}
    </Card>
  );
}

/* What leaves the stick: the running system as an update file, and the whole flash. */
function Copies() {
  const { t } = useI18n();
  const firmware = useFirmware();
  const confirm = useConfirm();
  const [copied, setCopied] = useState(false);
  return (
    <Card title={t("system.copies")}>
      <div className="grid gap-1">
        <a className="btn btn-sm w-fit" href="/api/firmware/download" download>
          <Download className="size-4" />
          {t("system.download")}
        </a>
        <Hint>{t("system.downloadHint")}</Hint>
      </div>
      <div className="grid gap-1">
        <button
          type="button"
          className="btn btn-sm w-fit"
          disabled={uploading(firmware.data)}
          onClick={() =>
            confirm.open({
              title: t("system.copyFlash"),
              body: t("system.copyFlashConfirm"),
              action: t("system.copyAction"),
              run: async (secret) => {
                setCopied(false);
                await downloadFlash(secret);
                setCopied(true);
              },
            })
          }
        >
          <HardDrive className="size-4" />
          {t("system.copyFlash")}
        </button>
        <Hint>{t("system.copyFlashHint")}</Hint>
      </div>
      {copied && <SaveState state="ok" message={t("system.copied")} />}
      {confirm.dialog}
    </Card>
  );
}

function HowUpdate() {
  const { t } = useI18n();
  return (
    <Card title={t("system.howUpdate")}>
      <ol className="grid list-decimal gap-2 pl-5 text-sm">
        <li>{t("system.step1")}</li>
        <li>{t("system.step2")}</li>
        <li>{t("system.step3")}</li>
        <li>{t("system.step4")}</li>
      </ol>
    </Card>
  );
}

export function SoftwarePage() {
  const { t } = useI18n();
  return (
    <Page title={t("nav.software")} hint={t("system.softwareHint")}>
      <Stack>
        <Systems />
        <Columns>
          <Stack>
            <Update />
            <Copies />
          </Stack>
          <Stack>
            <HowUpdate />
          </Stack>
        </Columns>
      </Stack>
    </Page>
  );
}
