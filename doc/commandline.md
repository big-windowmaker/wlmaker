# Commandline Reference for wlmaker {#commandline}

# Commandline Reference {#commandline_reference}

```
--start_xwayland : Optional: Whether to start XWayland. Disabled by default.
--lazy_xwayland : Optional: With --start_xwayland, starts the XWayland server only when the first X11 client connects. Disabled by default.
--config_file : Optional: Path to a configuration file. If not provided, wlmaker will scan default paths for a configuration file, or fall back to a built-in configuration.
--state_file : Optional: Path to a state file, with state of workspaces, dock and clips configured. If not provided, wlmaker will scan default paths for a state file, or fall back to a built-in default.
--theme_file : Optional: Path to a "theme" file, configuring the visual style for elements. If not provided, wlmaker will use a built-in default theme.
--root_menu_file : Optional: Path to a file describing the root menu. If not provided, wlmaker will use a built-in definition for the root menu.
--log_level : Log level to apply. One of DEBUG, INFO, WARNING, ERROR.
    Enum values:
      DEBUG (0)
      INFO (1)
      WARNING (2)
      ERROR (3)
--height : Desired output height. Applies when running in windowed mode, and only if --width is set, too. Set to 0 for using the output's preferred dimensions.
--width : Desired output width. Applies when running in windowed mode, and only if --height is set, too. Set to 0 for using the output's preferred dimensions.
```

* `--start_xwayland` , `--start_xwayland=true`: Starts *XWayland*. Permits to
  run X11 client applications under wlmaker.

  `--nostart_xwayland`, `--start_xwayland=false`: Do not start *XWayland*.

* `--lazy_xwayland`, `--lazy_xwayland=true`: Together with `--start_xwayland`,
  starts the *XWayland* server only when the first X11 client connects. The X11
  display socket is reserved at startup, so `DISPLAY` is set as usual. Saves
  the memory of an idle *XWayland* server in sessions without X11 clients.

  `--nolazy_xwayland`, `--lazy_xwayland=false`: Starts *XWayland* right away
  (default).

* `--config_file=<FILE>`: Loads the @ref config_file from `<FILE>`. Optional.
  If not provided, wlmaker will attempt to load the file from default
  locations, or fall back to use a compiled-in default.

* `--state_file=<FILE>`: Loads the *State* from `<FILE>`. Optional. If not
  provided, wlmaker will attempt to load the file from default locations, or
  fall back to use a compiled-in default.

* `--theme_file=<FILE>`: Loads the *Theme* from `<FILE>`. Optional. If not
  provided, wlmaker will attempt to load the file from default locations, or
  fall back to use a compiled-in default.

* `--root_menu_file=<FILE>`: Loads the contents of the root menu from `<FILE>`.
  If not provided, wlmaker will use a compiled-in default definition.

* `--log_level=<LEVEL>`: Optional, to adjust the log level. Logs are written to
  `stderr`. Use `--log_level=DEBUG` for most detailled output.

* `--height=HHH`, `--width=WWW`: Desired width and height for the output.
  Applies only when running in windowed mode (under X11 or Wayland). Both
  values must be provided to take effect.
