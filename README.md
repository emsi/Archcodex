# Archcodex

Archcodex is a planned native Linux desktop application for working with many
Codex sessions across local and remote app-server instances.

It will organize projects, working directories and Git worktrees by recent use,
list their conversations, search titles and conversation text, and embed the
existing Codex TUI. Session and project indicators will reflect **server events,
without status polling**.

The adopted foundation is **C++20, Qt 6 Widgets, KDE Frameworks 6 and Konsole
KPart**, distributed primarily as a **Flatpak**. Linux is the only target; KDE on
Debian 13 and Ubuntu 26.04 or newer is the initial desktop baseline. X11 support
is required. Wayland is optional.

## Design documents

- [Project design](docs/design.md): requirements, architecture, event handling,
  session lifetime, search, technology choices and validation.
- [Backend design](docs/backends.md): adapter contracts and the implementation
  sequence: existing local Unix sockets, remote TCP, SSH, then myCodex.
- [Flatpak design](docs/flatpak.md): runtime, terminal integration, host commands,
  proposed permissions and packaging checks.
- [Initial UI design](docs/ui.md): a simple IDE layout, expandable projects and
  sessions, and All/Active navigation tabs.

## Project status

Design documentation only. There is no application implementation, build system
or Flatpak manifest yet. **UI design is in progress**, starting with the project
navigator and terminal workspace.

The documents distinguish agreed requirements from proposed implementation
details and work that still needs validation. Upstream observations were checked
on **2026-11-08**; they are not a promise that every Codex version supports the
same protocol capabilities.

The project is intended to be open source. Its exact license and final Flatpak
application ID have not been selected.
