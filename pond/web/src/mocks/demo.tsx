// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

/*
 * The demo build's own part: the mock stick that answers /api in the browser,
 * and a line above the panel that says so and how to sign in. Its words are
 * here, not in the dictionaries, so that none of them reaches the stick.
 */

import { useI18n } from "../i18n";
import "./browser";

const WORDS = {
  en: "Demo: a stick simulated in your browser, with an example setup and root signed in. Where the panel asks for root's password, it is admin. Nothing leaves the page, and a reload starts over.",
  pl: "Demo: stick symulowany w przeglądarce, z przykładowymi ustawieniami i zalogowanym rootem. Gdy panel pyta o hasło roota, to admin. Nic nie wychodzi poza stronę, a odświeżenie zaczyna od nowa.",
};

export function DemoNotice() {
  const { lang } = useI18n();
  return (
    <div className="bg-info px-4 py-1 text-center text-info-content text-sm">{WORDS[lang]}</div>
  );
}
