// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * Operator, Phone: the phone service the OLT set up on the stick's phone
 * sockets, which the stick tells the OLT of (Operator, Identity) and which
 * have no wire. Its lines with their SIP accounts, the operator's SIP
 * server, the codecs and the VLAN of the voice, which goes through the SFP
 * lane to the host as a bridge: a VoIP device of the person's own, a
 * gateway for an analogue phone, an IP phone or a router that does VoIP,
 * registers with these in place of the ONT. The passwords show with root's
 * password. An OLT that sets the phone up through TR-069 sends none of it
 * through OMCI, which the page says.
 */

import { Link } from "@tanstack/react-router";
import { Copy, Eye } from "lucide-react";
import { useState } from "react";
import { useIdentity, usePhone, usePhoneSecrets } from "../api/config";
import type { Phone, PhoneSecrets } from "../api/types";
import { useConfirm } from "../components/Confirm";
import { Page } from "../components/Page";
import { QueryState } from "../components/Read";
import { Card, Cell, Facts, Row, Table } from "../components/ui";
import { useI18n } from "../i18n";
import { FibreState } from "./Fibre";
import { Columns, Hint, Stack } from "./system/shared";

/* The settings of a line as text, for a VoIP device's form or a note. */
function asText(p: Phone, shown: PhoneSecrets | null) {
  const s = p.server;
  const out: string[] = [];
  for (const l of p.lines) {
    const pw = shown?.lines.find((x) => x.port === l.port)?.secret;
    out.push(
      `# ${l.port}`,
      `number=${l.number ?? ""}`,
      `user=${l.user ?? ""}`,
      ...(pw !== undefined ? [`password=${pw}`] : []),
      `display=${l.display ?? ""}`,
    );
  }
  if (s)
    out.push(
      `proxy=${s.proxy ?? ""}`,
      `outbound_proxy=${s.outbound_proxy ?? ""}`,
      `registrar=${s.registrar ?? ""}`,
      `domain=${s.domain ?? ""}`,
      `port=${s.port ?? ""}`,
      `transport=${s.transport ?? ""}`,
    );
  out.push(`codecs=${p.codecs.join(",")}`, `dtmf=${p.dtmf ?? ""}`);
  if (p.vlan) out.push(`vlan=${p.vlan.host ?? ""}`);
  return `${out.join("\n")}\n`;
}

function Lines({ phone }: { phone: Phone }) {
  const { t } = useI18n();
  const reveal = usePhoneSecrets();
  const confirm = useConfirm();
  const [shown, setShown] = useState<PhoneSecrets | null>(null);
  const [copied, setCopied] = useState(false);
  const any = phone.lines.some((l) => l.password_set);
  return (
    <Card title={t("phone.lines")}>
      {phone.lines.length === 0 ? (
        <p className="text-sm">{t("phone.noLines")}</p>
      ) : (
        <>
          <Table
            head={[
              t("phone.socket"),
              t("phone.number"),
              t("phone.user"),
              t("phone.secret"),
              t("phone.display"),
            ]}
          >
            {phone.lines.map((l) => {
              const pw = shown?.lines.find((x) => x.port === l.port)?.secret;
              return (
                <Row key={l.port}>
                  <Cell head={t("phone.socket")}>{l.port}</Cell>
                  <Cell head={t("phone.number")}>
                    <span className="font-mono">{l.number ?? "–"}</span>
                  </Cell>
                  <Cell head={t("phone.user")}>
                    <span className="font-mono">{l.user ?? "–"}</span>
                  </Cell>
                  <Cell head={t("phone.secret")}>
                    {pw ? (
                      <span className="font-mono">{pw}</span>
                    ) : (
                      t(l.password_set ? "phone.hidden" : "phone.none")
                    )}
                  </Cell>
                  <Cell head={t("phone.display")}>{l.display ?? "–"}</Cell>
                </Row>
              );
            })}
          </Table>
          <div className="flex flex-wrap gap-2">
            {any && !shown && (
              <button
                type="button"
                className="btn btn-sm"
                onClick={() =>
                  confirm.open({
                    title: t("phone.show"),
                    body: t("phone.showConfirm"),
                    action: t("phone.show"),
                    run: async (secret) => setShown(await reveal.mutateAsync({ secret })),
                  })
                }
              >
                <Eye className="size-4" />
                {t("phone.show")}
              </button>
            )}
            <button
              type="button"
              className="btn btn-sm"
              onClick={() =>
                void navigator.clipboard.writeText(asText(phone, shown)).then(() => setCopied(true))
              }
            >
              <Copy className="size-4" />
              {t(copied ? "phone.copied" : shown ? "phone.copyAll" : "phone.copyNoPasswords")}
            </button>
          </div>
        </>
      )}
      {confirm.dialog}
    </Card>
  );
}

function Server({ phone }: { phone: Phone }) {
  const { t } = useI18n();
  const s = phone.server;
  const dash = (v: string | number | null | undefined) => (v == null || v === "" ? "–" : v);
  return (
    <Card title={t("phone.server")}>
      <Facts
        rows={[
          [t("phone.method"), t(`phone.method_${phone.method}`)],
          s && [
            t("phone.proxy"),
            <span key="proxy" className="font-mono">
              {dash(s.proxy)}
            </span>,
          ],
          s && [
            t("phone.outbound"),
            <span key="outbound" className="font-mono">
              {dash(s.outbound_proxy)}
            </span>,
          ],
          s && [
            t("phone.registrar"),
            <span key="registrar" className="font-mono">
              {dash(s.registrar)}
            </span>,
          ],
          s && [
            t("phone.domain"),
            <span key="domain" className="font-mono">
              {dash(s.domain)}
            </span>,
          ],
          s && [
            t("phone.transport"),
            `${(s.transport ?? "–").toUpperCase()}${s.port ? `, ${t("phone.port")} ${s.port}` : ""}`,
          ],
          phone.codecs.length > 0 && [t("phone.codecs"), phone.codecs.join(", ")],
          phone.dtmf && [t("phone.dtmf"), t(`phone.dtmf_${phone.dtmf}`)],
          phone.vlan && [
            t("phone.vlan"),
            t("phone.vlanValue", { host: dash(phone.vlan.host), fibre: dash(phone.vlan.fibre) }),
          ],
        ]}
      />
    </Card>
  );
}

function HowTo({ phone }: { phone: Phone }) {
  const { t } = useI18n();
  return (
    <Card title={t("phone.howTo")}>
      <ol className="list-decimal space-y-1.5 pl-5 text-sm">
        <li>{t("phone.step1", { vlan: phone.vlan?.host ?? "–" })}</li>
        <li>{t("phone.step2")}</li>
        <li>{t("phone.step3")}</li>
      </ol>
      <Hint>{t("phone.howToHint")}</Hint>
    </Card>
  );
}

export function PhonePage() {
  const { t } = useI18n();
  const phone = usePhone();
  const identity = useIdentity();
  const sockets = identity.data?.pots_ports ?? 0;
  return (
    <Page title={t("nav.phone")} hint={t("phone.hint")} actions={<FibreState />}>
      <QueryState reads={phone}>
        {() =>
          phone.data &&
          (sockets === 0 ? (
            <Card title={t("phone.noSocketsTitle")}>
              <p className="text-sm">
                {t("phone.noSockets")}{" "}
                <Link to="/operator/identity" className="link link-primary">
                  {t("nav.identity")}
                </Link>
              </p>
            </Card>
          ) : phone.data.method === "tr069" ? (
            <Card title={t("phone.tr069Title")}>
              <p className="text-sm">{t("phone.tr069")}</p>
            </Card>
          ) : (
            <Columns wide="left">
              <Stack>
                <Lines phone={phone.data} />
              </Stack>
              <Stack>
                <Server phone={phone.data} />
                <HowTo phone={phone.data} />
              </Stack>
            </Columns>
          ))
        }
      </QueryState>
    </Page>
  );
}
