// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/* How a channel (a GEM port) reads to people: its direction and its use. */

import type { GemPort } from "../api/types";
import { useI18n } from "../i18n";
import type { GemView } from "../lib/state";

export function useGemText() {
  const { t } = useI18n();

  const direction = (g: GemPort) =>
    g.multicast
      ? t("fibre.multicast")
      : g.direction === "both"
        ? t("fibre.both")
        : g.direction === "downstream"
          ? t("fibre.downstream")
          : t("fibre.upstream");

  const use = (v: GemView) =>
    v.use === "session"
      ? v.vlan
        ? t("fibre.useSession", { vlan: v.vlan })
        : t("fibre.useSessionNoVlan")
      : v.use === "bridged"
        ? t("fibre.useBridged", { bridge: v.bridge ?? "" })
        : t("fibre.useEnds");

  return { direction, use };
}
