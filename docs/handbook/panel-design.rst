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
and no colours or sizes of its own.

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
(``IPv4 and IPv6``, ``soon``), in the neutral tone.

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
(``rounded-field``). There are no shadows. A free place that can be taken is a
card with a dashed border.

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
