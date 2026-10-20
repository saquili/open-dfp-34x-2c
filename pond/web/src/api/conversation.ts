// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The conversation with the OLT as it goes on: the frames omcid keeps
 * (pond's value "omci_frames"), read from the number after the last one the
 * panel has, every two seconds, and at once again while a read comes back
 * full. A frame that changes the MIB has the MIB read again. omcid that
 * started again numbers its frames from 0, and the panel starts over. A read
 * that fails keeps the frames, says why, and is tried again two seconds on.
 */

import { useQueryClient } from "@tanstack/react-query";
import { useEffect, useState } from "react";
import { CHANGES } from "../lib/omci-frames";
import { request } from "./client";
import type { OmciFrame, OmciFrames } from "./types";

/* The frames the panel keeps, as omcid does, and a read's most. */
const KEPT = 1024;
const PAGE = 200;
const EVERY_MS = 2000;

export interface Conversation {
  frames: OmciFrame[];
  /** The oldest frame omcid keeps, and the next one's number. */
  first: number;
  head: number;
  /** omcid has not answered yet. */
  loading: boolean;
  /** Why the last read failed (no omcid, or no stick), or null when it answered. */
  error: unknown;
}

/* A frame of the OLT's, carried out, that changes the MIB. */
function changes(f: OmciFrame): boolean {
  return f.verdict === 4 && CHANGES.includes(f.type);
}

export function useConversation(): Conversation {
  const qc = useQueryClient();
  const [state, setState] = useState<Conversation>({
    frames: [],
    first: 0,
    head: 0,
    loading: true,
    error: null,
  });

  useEffect(() => {
    let since = 0;
    let kept: OmciFrame[] = [];
    let timer: ReturnType<typeof setTimeout> | undefined;
    let stopped = false;

    const read = async () => {
      let full = false;
      try {
        const v = (await request<{ value: OmciFrames }>(`/api/get/omci_frames?since=${since}`))
          .value;
        if (stopped) return;
        const last = () => kept.at(-1)?.seq ?? -1;
        const again = v.head < since || (kept.length > 0 && (v.frames[0]?.seq ?? since) < since);
        if (again) kept = [];
        const fresh = v.frames.filter((f) => f.seq > last());
        if (fresh.length || again) kept = [...kept, ...fresh].slice(-KEPT);
        since = kept.length ? last() + 1 : v.first;
        full = v.frames.length >= PAGE;
        if (fresh.some(changes)) void qc.invalidateQueries({ queryKey: ["omci"] });
        /* The same list while nothing came, so that nothing is worked out again. */
        const frames = kept;
        setState((s) =>
          s.frames === frames && s.head === v.head && !s.loading && s.error == null
            ? s
            : { frames, first: v.first, head: v.head, loading: false, error: null },
        );
      } catch (e) {
        if (stopped) return;
        setState((s) => ({ ...s, loading: false, error: e }));
      }
      if (!stopped) timer = setTimeout(read, full ? 0 : EVERY_MS);
    };

    void read();
    return () => {
      stopped = true;
      clearTimeout(timer);
    };
  }, [qc]);

  return state;
}
