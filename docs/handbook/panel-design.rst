.. SPDX-License-Identifier: GPL-2.0-only
.. SPDX-FileCopyrightText: 2026 Paweł Phan <szop.hektar_1w@icloud.com>

================
The panel's look
================

The stick's panel (``pond/web``) is a tool for people who know networks: it
shows a lot of technical values at once, and they read it to find one of them
or to see that all is well. These rules give every page the same look, so that
a thing found on one page is recognised on the next. They hold for every page;
a page that needs what they do not give changes the rules here first, and then
every page the change touches.

The themes' colours are tokens in ``pond/web/src/styles.css``, defined for the
light and for the dark theme; the shared pieces (a card, a label and its value,
a state, a figure) are in ``pond/web/src/components/ui.tsx``. A page uses those
and no colours or sizes of its own: no colour of Tailwind's palette
(``bg-white``, ``text-gray-500``), no colour written out in a style or in a
drawing. A drawing takes its colours from the tokens too (``var(--color-…)``,
or ``currentColor`` under a class of the theme), so that every rule here holds
in both themes.

Where the rules come from
=========================

The rules are those the design systems of large technical consoles agree on,
measured against the panel as it stood on 2026-10-09:

* AWS Cloudscape, the design system of the AWS console: key-value pairs, the
  status indicator, typography, spacing, content density, the details page and
  the card view (``cloudscape.design``).
* IBM Carbon: typography, spacing and the status indicator pattern
  (``carbondesignsystem.com``).
* Nielsen Norman Group: visual hierarchy, the ten usability heuristics, the
  common region (``nngroup.com``); the Laws of UX (``lawsofux.com``).
* Refactoring UI by Adam Wathan and Steve Schoger: hierarchy, labels, borders,
  the spacing system.

The panel then had thirteen sizes of text, three of them under 12 px; ten
shades of grey text, seven of which fall short of 4.5:1 against a card of the
light theme; titles in capitals in three styles; and fixed-width type in three
sizes, used for some addresses and not for others.

Text
====

Five sizes, no other:

.. list-table::
   :header-rows: 1
   :widths: 12 16 72

   * - Size
     - Class
     - For
   * - 12 px
     - ``text-xs``
     - A value's label, a hint under a field, a note under a value, an error
       under a field.
   * - 14 px
     - ``text-sm``
     - Text and values; a section's title within a card; a field's label.
   * - 16 px
     - ``text-base``
     - A card's title.
   * - 24 px
     - ``text-2xl``
     - A page's title.
   * - 30 px
     - ``text-3xl``
     - A big figure, the one a card is about (light received, a rate).

Nothing is smaller than 12 px, a chart's axis included.

Two weights: 400 for text and values, 600 for titles (a page's, a card's, a
section's, a field's label) and for buttons.

Two colours of text, and a third that carries nothing to read:

.. list-table::
   :header-rows: 1
   :widths: 18 26 56

   * - Colour
     - Class
     - For, and its contrast (light theme / dark theme)
   * - Main
     - ``text-base-content``
     - Titles, values, text: 15.6:1 / 13.7:1.
   * - Secondary
     - ``text-base-content/70``
     - Labels, hints, notes, units, a description under a title: 5.9:1 /
       7.3:1 against a card, 5.7:1 / 7.6:1 against the page.
   * - Faint
     - ``text-base-content/45``
     - A field's placeholder and a disabled control, only: what is in it is
       never information.

No shade between them: ``/60`` reads at 4.3:1 against a card of the light
theme, ``/55`` at 3.7:1 and ``/50`` at 3.2:1, all short of the 4.5:1 that text
needs; a thing that is off (a rule, a forward) is said in words, and its row
keeps the secondary colour.

Words are written as a sentence is, a capital for the first word and for names
only: no title written in capitals.

Values
======

Fixed-width type (``font-mono``), at the size of the text around it, for
whatever is read character by character: an IP address, a prefix, a MAC
address, an interface's name (``wan100``, ``ppp0``), an identifier (a managed
entity's, a GEM port's when written alone), a hexadecimal number. Not for
words, nor for a number that is a quantity.

Every figure is set in figures of one width (``tabular``), so that figures that
change do not jitter and figures one under another line up.

A label and its value
---------------------

The label stands above its value, at 12 px in the secondary colour; the value
under it at 14 px in the main colour, 4 px below. A label is one to three
words. Pairs stand in columns that have the same width in every card of a page,
so that the same value of one card stands over the same value of the next.

A value without anything in it is ``–``. A value is never cut short (no
ellipsis): a long one wraps, at a comma or a space only, never within an
address. A word about a value (``shared with session 1``) is a note: a line of
its own under the value, at 12 px in the secondary colour, not brackets after it.

A section within a card has a title at 14 px, weight 600, in the main colour,
above its pairs.

Tables
------

Several things of one kind stand in a table, a row a thing, and not as a card
each that repeats its labels: the names of the columns stand once, in the
table's head, at 12 px, weight 600, in the secondary colour. A cell holds its
value on its first line (14 px, main colour) and, under it, lines of detail
(12 px, secondary) that say what they are in their own words (``MTU 1500``,
``DNS 198.51.100.53``) rather than under a label of their own. Cells are
16 px from the sides and 12 px from the lines, and their content starts at
their top; a line of 1 px parts the rows. A row's actions stand at its end and
are always shown. On a narrow screen a row stands as a list of its cells, each
under its column's name. ``Table``, ``Row`` and ``Cell`` in ``ui.tsx`` are
these pieces.

State
=====

The state of a thing is a dot in the state's colour and a word in the main
colour; never the colour alone, and one or two words. The word is not coloured:
against a card of the light theme the yellow reads at 2.5:1 and the green at
4.4:1, short of the 4.5:1 that text needs.

.. list-table::
   :header-rows: 1
   :widths: 16 22 62

   * - Colour
     - Token
     - For
   * - Green
     - ``success``
     - Works: up, connected, running.
   * - Yellow
     - ``warning``
     - Under way, or working but near its limit: connecting, weak light.
   * - Red
     - ``error``
     - Does not work, or wants a hand: no link, failed.
   * - Grey
     - secondary text
     - Off, unused, not set, not known yet.

A pill (``Badge``) is not a state: it marks a neutral property of a thing
(``IPv4 and IPv6``, ``soon``), in the neutral tone. Where a pill does carry a
state, it is the state's dot and word inside the pill's outline, tinted by the
state's colour, its word in the main colour.

The same holds wherever a state's colour meets words: a notice over a page or
a card (``alert-soft``) has its icon in the state's colour and its words in the
main colour; so has the line under a form that says the change was saved, and
a figure in a state (light that is weak) stands in the main colour after the
state's dot. The red of an error is the one exception: an error's words are in
``error``, which reads at 5.2:1 against a card of the light theme and at 6.5:1
in the dark one.

.. list-table:: The states' colours as text, against a card
   :header-rows: 1
   :widths: 22 26 26 26

   * - Token
     - Light theme
     - Dark theme
     - As words
   * - ``success``
     - 4.4:1
     - 8.5:1
     - no
   * - ``warning``
     - 2.5:1
     - 9.8:1
     - no
   * - ``info``
     - 3.8:1
     - 7.6:1
     - no
   * - ``error``
     - 5.2:1
     - 6.5:1
     - yes
   * - ``primary``
     - 6.0:1
     - 6.9:1
     - links and the chosen option

A button in a state's colour is no exception: a page's main action is in the
accent (``primary``), whatever it does (keep a mode on trial, start a system).

What the stick has no software for yet is not a state of a thing but a gap in
the stick: the page or form says so in one dashed notice (``NotYet``), titled
"Not on the stick yet", that says what it would be, in place of the list and
the form, with no count in the menu and no button that saves. The panel knows
it by the part missing from the stick's settings (``served`` in
``lib/state.ts``), so the notice goes by itself when the software has it.

Colour
======

Text and lines are neutral. The accent (``primary``) is for what acts and what
is chosen: a button that does the page's action, a link, a chosen option, the
page the menu is on. The states' colours are for states and for errors; a
colour is never decoration.

Space
=====

Every distance is one of 4, 8, 12, 16, 24 and 32 px (Tailwind's 1, 2, 3, 4, 6
and 8), so that the eye finds things on the same lines:

.. list-table::
   :header-rows: 1
   :widths: 46 18 36

   * - Between
     - Distance
     - Class
   * - A label and its value; a field's label and the field
     - 4 px
     - ``gap-1``, ``mt-1``
   * - A value and its note; icons and their words
     - 4–8 px
     - ``gap-1``, ``gap-2``
   * - Pairs, one under another and side by side
     - 12–16 px
     - ``gap-3``, ``gap-4``
   * - Fields of a form
     - 16 px
     - ``gap-4``
   * - Sections of a card
     - 24 px
     - ``gap-6``
   * - A card's edge and its content
     - 16 px
     - ``p-4``
   * - Cards
     - 12 px
     - ``gap-3``
   * - A page's title and its cards
     - 16 px
     - ``mb-4``

What belongs together is near; what does not is further. The space groups, not
lines: within a card, sections have neither borders nor backgrounds of their
own.

Edges
=====

A card has a border of 1 px (``border-base-300``) and corners of 12 px
(``rounded-box``); a field, a button and an option have corners of 8 px
(``rounded-field``). There are no shadows, not even under a menu that drops
down or a tip over a chart: they have a card's border, which holds in the dark
theme, where a shadow is not seen. A free place that can be taken is a card
with a dashed border.

Actions
=======

A card's actions (change, remove) are icons in the secondary colour, of at
least 32 px to the touch; an action that destroys turns red when the pointer is
on it, and asks before it acts. A page's main action is a button in the accent;
an action beside it is a button without a fill.

Forms
=====

A field's label stands above it (14 px, weight 600), a hint under it (12 px,
secondary), and an error in its place (12 px, ``error``). A choice of a few
answers that each need a sentence is a set of options, one beside another, the
chosen one bordered in the accent; a choice of a few short words is a row of
buttons, the chosen one filled. The buttons stand at the form's end, the one
that saves first.

The panel's own settings
========================

The theme and the language are settings of the panel in one browser, not of
the stick: they stand in the bar over every page, at its right, after the time
and before the user's menu, each a menu behind a small button that shows the
choice: the language's code, the theme's icon, named ``Theme: Dark`` for those
who do not see it. The browser keeps them (``localStorage``);
without it a choice lasts the visit. A setting of the stick never stands
there: those are pages, kept on the stick (System, Settings).

The theme is the system's colour scheme until the reader chooses Light or
Dark. The choice is on the page before its first paint, so that a dark page
never flashes light: ``src/theme-boot.js`` puts it there, a script of its own
that the page loads before anything else, since pond's
Content-Security-Policy runs no script written into the page.

Reads and changes
=================

Every value a page shows comes from a read of the stick's, and most reads are
made again every few seconds. A page shows a read's states the same way as
every other page (``pond/web/src/components/Read.tsx``):

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - State
     - What the page shows
   * - First load
     - A spinner and "Reading from the stick…" where the values will stand
       (``Reading``). The live figures, which every page shows, stand in for
       the page until their first answer, and who is signed in for the panel.
   * - Failed, nothing to show
     - Why, in red, and *Try again*, in place of the values (``QueryState``,
       ``QueryNote``). For the live figures, the bar over the page says it:
       the stick does not answer, or it answered without its state.
   * - Failed, an answer before
     - The last answer stays, and a line over it says that the last read
       failed, why, the time of the answer shown, and *Try again*. A read
       made every few seconds goes on being made; the line goes with the
       next answer.
   * - Read again
     - Nothing on the page: a read that takes longer than a poll usually does
       shows a spinner in the bar over every page (``Busy``), so that a poll
       never makes a page move.

A read that only adds to what a page shows (the devices' names beside the
forwards, the interfaces beside the connections) says nothing while it is
read, only that it failed. A value the stick has no software for, which pond
does not know (404), is said in words of its own, not as a failure.

A change of a setting, once asked for:

* while it is sent, its button is disabled and the line at the form's end
  says "Saving…" with a spinner; a second click sends nothing;
* when pond refuses it, or the stick does not answer, why, in red, where it
  was asked for: at the form's end, or in the row whose button asked (remove,
  move), or in the notice whose button asked (keep, confirm); pond gives one
  reason for a change, so a field's error under the field is the panel's own
  check, made before anything is sent;
* when it is taken, the form says *Saved* with a tick, or closes, or the row
  goes; and the panel reads again exactly what the change makes stale, as
  ``REFRESH`` in ``pond/web/src/api/config.ts`` lists it, the reads on the
  page at once. Signing in and out forgets every read of the session before.
