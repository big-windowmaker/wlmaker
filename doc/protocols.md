# Wayland Protocol support {#protocols}

# Wayland Protocol support

This page documents which of the Wayland protocols (beyond the core
[Wayland protocol](https://wayland.app/protocols/wayland)) are supported
by wlmaker.

# Idle inhibit

Permits inhibiting the idle behaviour, such as locking the screen.

* Status: Implemented, untested.
* Reference: https://wayland.app/protocols/idle-inhibit-unstable-v1

# Presentation time

Provides feedback to clients about when their content was shown on an output.
Used by video players and toolkits for smooth animation and A/V sync.

* Status: Supported. Feedback is sent by the wlroots scene graph.
* Reference: https://wayland.app/protocols/presentation-time

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

# wlr layer shell {#protocols_wlr_layer_shell}

Clients can use this interface to associate `wl_surface` with a *layer* of the
output. The surfaces may slo be anchored to edges and/or corners of a screen,
specify input semantics and will be rendered above or below desktop elements,
as configured.

* Status: Supported, except popups and input semantics.
* Reference: https://wayland.app/protocols/wlr-layer-shell-unstable-v1

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

# XDG shell

Interface for creating desktop-style surfaces.

* Status: Supported, with a few gaps.
* Reference: https://wayland.app/protocols/xdg-shell
