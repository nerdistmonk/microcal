# Microcal

A lightweight, keyboard-driven terminal calendar for Linux built with C and Ncurses. Microcal provides an intuitive monthly grid, event management, theme support, and strict RFC 5545 iCalendar (`.ics`) import and export capabilities.

![Alt text](shot.png)

---

## Features

- **Terminal Grid View:** Clean monthly layout with highlighting for current date, selection cursor, and active event indicators (`[DD*]`).
- **RFC 5545 Standard Compliant:**
  - Strict vanilla `.ics` export with no proprietary vendor junk or tracking attributes.
  - Generates mandatory `UID` and UTC `DTSTAMP` timestamps.
  - Adheres to standard all-day non-inclusive `DTEND` calculations.
  - Multi-line text escaping and unescaping (`\n`).
  - Supports recurring events (`RRULE` with daily, weekly, monthly, and yearly frequencies, intervals, weekday specifications including ordinals such as `1MO` or `-1FR`, and `UNTIL` / `COUNT` limits).
- **Interactive Event Dialogs:** View, inspect, scroll through multi-line descriptions, and delete events directly from the UI.
- **Theming:** Built-in color themes to fit different terminal palettes.
- **Session Persistence:** Automatically remembers and reloads the last used `.ics` file.

---

## Dependencies

- A C99 compiler (`gcc` or `clang`)
- `make`
- `ncurses` development libraries (`libncurses-dev` on Debian/Ubuntu, `ncurses-devel` on Fedora/RHEL/Arch)

---

## Building and Running

Clone or navigate to the source directory and compile:

Just run 'build_release' and it will build via cmake into a .build directory.

---

## Keybindings

| Key | Action |
| :--- | :--- |
| **Arrow Keys** | Navigate calendar days (Left, Right, Up, Down) |
| **Page Up / Page Down** | Switch between previous and next months |
| **Enter** / **v** | View scheduled events and details for the selected day |
| **e** | Add an event on the selected day, with an optional repeat (see [Adding Events](#adding-events)) |
| **s** | Save / export current calendar to `.ics` |
| **F1** | Open Help and Keyguide dialog |
| **F2** | Import an external `.ics` file (events whose UID already exists are replaced) |
| **F3** | Export current calendar to an alternate `.ics` path |
| **t** | Cycle color themes (Default, Ocean, Foreground Pop) |
| **q** | Quit Microcal |

### Day Event View Controls

When viewing events on a specific day (**Enter**):
- **Up / Down**: Select between events on that day
- **d** / **Delete**: Delete the currently selected event (asks for confirmation)
- **Enter** / **Esc** / **q**: Close the event viewer

### Adding Events

Press **e** to add an event on the selected day:
1. Type the event title and press **Enter**.
2. Choose a repeat option with **Left / Right** (or **Up / Down**) and press **Enter**:
   - `< No repeat >`: a single event.
   - `< Repeat on every <weekday> >`: weekly on that weekday.
   - `< Repeat on every <N>th of the month >`: monthly on that day number.
   - `< Repeat on every <N>th <weekday> of the month >`: monthly on the same ordinal weekday (for example the 1st Wednesday). Days 29 to 31 count as the 5th occurrence, and months without a 5th occurrence are skipped.
   - `< Repeat once a year >`: yearly on the same date.
3. For repeating events, a checkbox dialog appears (**Up / Down** to move, **Space** to toggle, **Enter** to confirm):
   - `[ ] Every 2 weeks` (or months or years): when checked, the event repeats every second week (month, year). When unchecked, it repeats every week (month, year).
   - `[ ] Ends on a date`: when unchecked, the event repeats forever. When checked, you enter an end date as `YYYY-MM-DD`.

**Esc** at any step cancels the event. Repeating events are saved in the `.ics` export as standard `RRULE` rules.

---

## File Format & Standards

Microcal produces standard iCalendar files compatible with Google Calendar, Apple Calendar, Thunderbird, and Nextcloud.

```text
BEGIN:VCALENDAR
VERSION:2.0
PRODID:-//Microcal//Ncurses Calendar Linux//EN
CALSCALE:GREGORIAN
BEGIN:VEVENT
UID:microcal-1793567611-1@local
DTSTAMP:20261101T211331Z
SUMMARY:Team Sync
DTSTART;VALUE=DATE:20261115
DTEND;VALUE=DATE:20261116
TRANSP:TRANSPARENT
END:VEVENT
END:VCALENDAR
```

### Import Behaviour

- **Replace by UID:** An imported event whose `UID` matches an existing event replaces it. Events absent from the file are kept.
- **Missing UID:** An event without a `UID` gets a stable content-hash UID, so re-importing the same file does not duplicate it.
- **Duplicate UIDs in one file:** Later copies get a `#N` suffix so that no event is lost.
- **Line folding:** Folded lines (RFC 5545 §3.1) are unfolded before parsing. Export folds at 75 octets, never splitting a UTF-8 character.
- **Sub-daily frequencies** (`HOURLY`, `MINUTELY`, `SECONDLY`) are imported as `DAILY`.
- **Durations:** `DURATION` is used when `DTEND` is absent.
- **Alarms:** `VALARM` blocks are skipped, so they cannot overwrite the event's `SUMMARY` or `DESCRIPTION`.
- **Timed events** become all-day events on their start date, since Microcal stores dates only. The time is not kept.

**Not supported** (the rule is imported without these parts): `RECURRENCE-ID`, `EXDATE`, `RDATE`, `BYSETPOS`, `BYHOUR`, negative `BYMONTHDAY`, and multiple values in `BYMONTH` or `BYMONTHDAY` (only the first value is used).

---

## Small Terminals

- The day event view needs at least 50 columns and 9 lines. Below that it shows "Terminal too small" and keeps working when the window grows again.
- Text prompts and the repeat picker need at least 60 columns and 7 lines.
- **Esc** cancels a text prompt. Previously Esc was entered as text.

---

## License

MIT License. Feel free to modify and distribute.