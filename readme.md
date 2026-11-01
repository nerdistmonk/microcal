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
  - Supports recurring events (`RRULE` with daily, weekly, monthly, and yearly frequencies, intervals, and weekday specifications).
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
| **e** | Add a new event on the selected day |
| **s** | Save / export current calendar to `.ics` |
| **F1** | Open Help and Keyguide dialog |
| **F2** | Import an external `.ics` file |
| **F3** | Export current calendar to an alternate `.ics` path |
| **t** | Cycle color themes (Default, Ocean, Foreground Pop) |
| **q** | Quit Microcal |

### Day Event View Controls

When viewing events on a specific day (**Enter**):
- **Up / Down**: Select between events on that day
- **d** / **Delete**: Delete the currently selected event
- **Enter** / **Esc** / **q**: Close the event viewer

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

---

## License

MIT License. Feel free to modify and distribute.