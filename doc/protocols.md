# Wayland Protocol support {#protocols}

# Wayland Protocol support

This page documents which of the Wayland protocols (beyond the core
[Wayland protocol](https://wayland.app/protocols/wayland)) are supported
by wlmaker.

# Idle notify

Lets clients get notified when the user is idle for a given time, and when
activity resumes. Used by idle management daemons such as
[swayidle](https://github.com/swaywm/swayidle).

* Status: Supported. Activity is reported from keyboard and pointer input.
  Idle notifications are inhibited while an idle inhibitor is active.
* Reference: https://wayland.app/protocols/ext-idle-notify-v1

# Idle inhibit

Permits inhibiting the idle behaviour, such as locking the screen.

* Status: Implemented, untested.
* Reference: https://wayland.app/protocols/idle-inhibit-unstable-v1

# Presentation time

Provides feedback to clients about when their content was shown on an output.
Used by video players and toolkits for smooth animation and A/V sync.

* Status: Supported. Feedback is sent by the wlroots scene graph.
* Reference: https://wayland.app/protocols/presentation-time

# Relative pointer

Sends relative pointer motion (unaccelerated, and not limited by the screen
edges) to the client that has pointer focus. Used by games and by 3D or
remote desktop applications.

* Status: Supported. Without pointer constraints, the cursor still moves.
* Reference: https://wayland.app/protocols/relative-pointer-unstable-v1

# Session lock

Allows privileged Wayland clients to lock the session and display arbitrary
graphics while the session is locked. An example of such a client is
[swaylock](https://github.com/swaywm/swaylock).

* Status: Supported. If the lock client dies without unlocking, the session
  remains locked, and a new lock client can take over. A new lock is refused
  (`finished`) while a lock is held.
* Reference: https://wayland.app/protocols/ext-session-lock-v1

# Single-pixel buffer

Lets clients create a buffer for a single, solid color pixel. Cheap way to
draw backgrounds and solid areas, eg. by GTK4 and video players.

* Status: Supported.
* Reference: https://wayland.app/protocols/single-pixel-buffer-v1

# Virtual keyboard

Permits clients to emulate a keyboard, with a keymap of their choice. Used by
on-screen keyboards, `wtype` and remote desktop servers (`wayvnc`).

* Status: Supported.
* Reference: https://wayland.app/protocols/virtual-keyboard-unstable-v1

# wlr gamma control

Permits clients to set the gamma tables of an output. Used by "night light"
tools such as `wlsunset` or `gammastep`.

* Status: Supported. Applied through the wlroots scene graph, either via the
  output's gamma LUT or, if the output has none, when rendering.
* Reference: https://wayland.app/protocols/wlr-gamma-control-unstable-v1

# wlr layer shell {#protocols_wlr_layer_shell}

Clients can use this interface to associate `wl_surface` with a *layer* of the
output. The surfaces may slo be anchored to edges and/or corners of a screen,
specify input semantics and will be rendered above or below desktop elements,
as configured.

* Status: Supported, except popups and input semantics.
* Reference: https://wayland.app/protocols/wlr-layer-shell-unstable-v1

# wlr output power management

Permits clients to power outputs off and on (DPMS), for example
`wlopm --off '*'` triggered from `swayidle`.

* Status: Supported. Powering off disables the output, but keeps it in the
  output layout. Outputs disabled through configuration are not affected.
* Reference: https://wayland.app/protocols/wlr-output-power-management-unstable-v1

# wlr virtual pointer

Permits clients to emulate a pointer device, eg. `wlrctl`, `ydotool`-like
tools, on-screen pointer helpers or remote desktop servers (`wayvnc`).

* Status: Supported.
* Reference: https://wayland.app/protocols/wlr-virtual-pointer-unstable-v1

# wlr screencopy

This protocol allows clients to ask the compositor to copy part of the screen
content to a client buffer.

* Status: Supported.
* Reference: https://wayland.app/protocols/wlr-screencopy-unstable-v1

# wlr output management {#protocols_wlr_output_management}

Implements interfaces to obtain and modify output device configruation. It
permits thirdparty clients (such as `wlr-randr`, `wdisplays`) to obtain a list
of available outputs, and set their propertis.

* Status: Supported.
* Reference: https://wayland.app/protocols/wlr-output-management-unstable-v1

# XDG activation

Permits clients to pass focus to another surface, for example to raise an
existing window when an application is launched a second time, or when a
launcher hands over an activation token.

* Status: Supported. Any valid token is accepted. Windows on a workspace
  other than the current one are not activated.
* Reference: https://wayland.app/protocols/xdg-activation-v1

# KDE server decoration

The obsolete predecessor of XDG decoration (`org_kde_kwin_server_decoration`).
GTK3 does not implement XDG decoration, but uses this protocol to ask for
server-side decorations when not drawing its own header bar.

* Status: Supported. The default mode follows the `Decoration` `Mode` setting
  of the configuration file: `SuggestServer` and `EnforceServer` announce
  server-side, `SuggestClient` and `EnforceClient` client-side decoration.
  The mode requested by a client is applied like for XDG decoration.
* Reference: https://wayland.app/protocols/kde-server-decoration

# XDG decoration

This interface allows a compositor to announce support for server-side
decorations. A client can use this protocol to request being decorated by
wlmaker.

* Status: Supported.
* Reference: https://wayland.app/protocols/xdg-decoration-unstable-v1

# XDG foreign

Permits a client to export a handle for one of its toplevels, and another
client to import it and set it as parent of its own toplevel. Used by
`xdg-desktop-portal` to attach file chooser dialogs to the application window.

* Status: Supported (unstable v1 and v2). The parent relation is passed on as
  `set_parent`, which wlmaker does not act upon yet.
* Reference: https://wayland.app/protocols/xdg-foreign-unstable-v2

# XDG shell

Interface for creating desktop-style surfaces.

* Status: Supported, with a few gaps.
* Reference: https://wayland.app/protocols/xdg-shell
