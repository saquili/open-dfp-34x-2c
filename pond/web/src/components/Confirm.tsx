// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The one confirmation of the panel (docs/handbook/panel-design.rst,
 * Confirmation): a dialog over the page, opened by the button of an action
 * that cannot be taken back or that needs root's password, which says what
 * the action does and what follows from it. For root's password it asks at
 * that moment: its field takes the focus, Enter does the action, and the
 * password goes with the request and is forgotten with the dialog, so that
 * no page keeps a field of it. Until the stick has answered the dialog stays,
 * busy; a wrong password, a stop after too many of them and any other
 * refusal are said in it, and it closes once the action is done, the page
 * showing what came of it. Escape and Cancel shut it while nothing is sent.
 */

import { type ReactNode, useEffect, useRef, useState } from "react";
import { useI18n } from "../i18n";
import { attempt } from "../lib/confirm";
import { SecretInput } from "./Field";

export interface Ask {
  /** What the action is, as its button names it. */
  title: string;
  /** What it does and what follows, in a sentence or two. */
  body?: ReactNode;
  /** The words of the button that does it. */
  action: string;
  /** Red, for what cannot be taken back. */
  danger?: boolean;
  /** Whether root's password goes with it: yes unless said. */
  rootPassword?: boolean;
  /** The action, with the password typed; done when it resolves. */
  run: (secret: string) => Promise<unknown>;
}

/**
 * The dialog of @ask while it is not null; @onClose sets it to null.
 * @children are shown under its words, as the page has them now: an
 * upload's progress.
 */
export function Confirm({
  ask,
  onClose,
  children,
}: {
  ask: Ask | null;
  onClose: () => void;
  children?: ReactNode;
}) {
  const { t } = useI18n();
  const dialog = useRef<HTMLDialogElement>(null);
  const field = useRef<HTMLInputElement>(null);
  const [secret, setSecret] = useState("");
  const [busy, setBusy] = useState(false);
  const [said, setSaid] = useState<{ text: string; atField: boolean } | null>(null);
  const needs = ask?.rootPassword !== false;

  useEffect(() => {
    const d = dialog.current;
    if (!d) return;
    if (ask) {
      setSecret("");
      setSaid(null);
      setBusy(false);
      if (!d.open) d.showModal();
      field.current?.focus();
    } else if (d.open) {
      d.close();
    }
  }, [ask]);

  const shut = () => {
    if (!busy) onClose();
  };

  async function submit(e: Event) {
    e.preventDefault();
    if (!ask || busy) return;
    setBusy(true);
    setSaid(null);
    const out = await attempt(t, ask.run, needs, secret);
    setBusy(false);
    if (out.done) {
      setSecret("");
      onClose();
      return;
    }
    setSaid(out);
    if (out.atField) {
      setSecret("");
      field.current?.focus();
    }
  }

  return (
    <dialog
      ref={dialog}
      className="modal modal-bottom sm:modal-middle"
      aria-labelledby="confirm-title"
      onCancel={(e) => {
        e.preventDefault();
        shut();
      }}
    >
      {ask && (
        <form
          className="modal-box grid gap-4 border border-base-300 p-5 shadow-none"
          onSubmit={submit}
          noValidate
        >
          <div className="grid gap-2">
            <h2 id="confirm-title" className="text-base font-semibold">
              {ask.title}
            </h2>
            {ask.body && <div className="grid gap-2 text-sm wrap-anywhere">{ask.body}</div>}
            {children}
          </div>
          {needs && (
            <div className="grid gap-1">
              <label htmlFor="confirm-password" className="text-sm font-semibold">
                {t("confirm.password")}
              </label>
              <SecretInput
                ref={field}
                id="confirm-password"
                autoComplete="current-password"
                value={secret}
                invalid={!!said?.atField}
                disabled={busy}
                onInput={(e) => setSecret((e.currentTarget as HTMLInputElement).value)}
              />
              {said?.atField ? (
                <p role="alert" className="text-xs text-error">
                  {said.text}
                </p>
              ) : (
                <p className="text-xs text-base-content/70">{t("confirm.passwordHint")}</p>
              )}
            </div>
          )}
          {said && !said.atField && (
            <p role="alert" className="text-sm text-error wrap-anywhere">
              {said.text}
            </p>
          )}
          <div className="flex flex-wrap items-center gap-2">
            <button
              type="submit"
              className={`btn btn-sm ${ask.danger ? "btn-error" : "btn-primary"}`}
              disabled={busy}
            >
              {busy && <span className="loading loading-spinner loading-xs" />}
              {ask.action}
            </button>
            <button type="button" className="btn btn-sm btn-ghost" disabled={busy} onClick={shut}>
              {t("confirm.cancel")}
            </button>
          </div>
        </form>
      )}
    </dialog>
  );
}

/**
 * A confirmation for a card or a page: its dialog, to be placed among what
 * the card shows, and the way to open it with an action.
 */
export function useConfirm(children?: ReactNode) {
  const [ask, setAsk] = useState<Ask | null>(null);
  return {
    open: setAsk,
    dialog: (
      <Confirm ask={ask} onClose={() => setAsk(null)}>
        {children}
      </Confirm>
    ),
  };
}
