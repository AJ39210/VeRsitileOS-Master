# VeRsitileOS native desktop library

`port/libx/` contains a new VeRsitileOS-specific C library inspired by Xlib's
display/window/event model. Build and install it with:

```sh
make -C Kernel libx
```

The build produces `system/lib/libx.a` and installs headers in
`system/include/X11/`. Applications include `<X11/Xlib.h>` and
`<X11/Xdesktop.h>` and link with `-lx -lc`.

The native API provides:

- A display context, root desktop, bounded client window surfaces, mapping,
  focus, geometry queries, event selection, and a window list for taskbars.
- Window stacking, fixed/topmost and no-focus surfaces, app-list exclusion,
  minimum sizes, iconify/restore/maximize operations, and up to 16 virtual
  workspaces.
- Per-window rectangle, line, image, and text drawing.
- A polling event queue for keyboard, pointer, focus, expose, and close-request
  events, including root desktop events and window configure/state changes.
- A configurable built-in manager with title bars, focus/raise behavior,
  dragging, resizing, and close/minimize/maximize controls. Each behavior can
  be disabled so an application can implement its own window manager.
- A small optional widget layer for labels, buttons, checkboxes, sliders,
  progress bars, and single-line text fields. Widgets are individually
  themed, stateful, and report activation, value-change, and submit events.
- Solid-color or centered pixel-image wallpaper and a software cursor with
  arrow, I-beam, hand, crosshair, and hidden shapes.

## Reusable controls

Include `<X11/Xwidgets.h>` to add standard controls to a window while keeping
the desktop's colors and event behavior yours to define:

```c
XWidgetSet *controls = XWidgetSetCreate(display, window);
XWidgetTheme theme = {
    .background = XRGB(35, 40, 52),
    .foreground = XRGB(245, 245, 250),
    .accent = XRGB(55, 125, 225),
    .border = XRGB(95, 105, 125),
    .disabled = XRGB(120, 120, 130)
};
XWidgetSetSetTheme(controls, &theme);
XWidget *launch = XWidgetAddButton(
    controls, 1, 16, 16, 112, 28, "Launch");
XWidget *autostart = XWidgetAddCheckbox(
    controls, 2, 16, 56, 180, 24, "Start at login", false);
XWidget *volume = XWidgetAddSlider(
    controls, 3, 16, 96, 200, 24, 0, 100, 50);
XWidget *status = XWidgetAddProgressBar(
    controls, 4, 16, 136, 200, 20, 0, 100, 25);
XWidget *search = XWidgetAddTextField(
    controls, 5, 16, 172, 240, 26, 64, "");
```

In the event loop, pass window events through `XWidgetSetHandleEvent` and then
handle returned widget IDs in the desktop/application. It reports button
activation, checkbox/slider value changes, and text-field submission; use
`XWidgetGetValue`/`XWidgetSetValue` and `XWidgetGetText`/`XWidgetSetText` to
connect controls to application state. Call `XWidgetSetDraw` after changes,
then `XFlush`. A widget set supports up to `X_WIDGET_MAX` controls with unique
IDs. Text fields currently accept printable ASCII, append at the end, support
backspace, and submit on Enter. For richer text editing, menus, icons, and
other specialized controls, applications can combine the existing drawing
primitives with the same event API.

```c
XEvent event;
while (XNextEvent(display, &event))
{
    XWidgetEvent action;
    if (XWidgetSetHandleEvent(controls, &event, &action))
    {
        if (action.type == X_WIDGET_EVENT_ACTIVATED && action.id == 1)
            start_application();
        else if (action.type == X_WIDGET_EVENT_CHANGED && action.id == 3)
            set_system_volume(action.value);
        else if (action.type == X_WIDGET_EVENT_SUBMITTED && action.id == 5)
            search_applications(action.text);
        XWidgetSetDraw(controls);
        XFlush(display);
    }
}
```

## Build your own shell and taskbar

Panels, launchers, status bars, and taskbars are ordinary undecorated windows.
Make them fixed, topmost, excluded from the taskbar's own app list, and
non-focusable. `XListWindows` and `XGetWindowAttributes` let a shell draw a
button for each application window, including windows on other workspaces or
that are currently minimized.

```c
Display *display = XOpenDisplay();
uint32_t screen_width, screen_height;
XGetScreenSize(display, &screen_width, &screen_height);
XWMSetFeatures(display, X_WM_DECORATIONS | X_WM_DRAG_WINDOWS |
                        X_WM_AUTO_FOCUS | X_WM_AUTO_RAISE |
                        X_WM_RESIZE_WINDOWS);

Window *taskbar = XCreateSimpleWindow(
    display, XDefaultRootWindow(display),
    0, (int32_t)screen_height - 40, screen_width, 40,
    XRGB(28, 32, 42), "Taskbar");
XSetWindowFlags(display, taskbar,
    X_WINDOW_UNDECORATED | X_WINDOW_ALWAYS_ON_TOP |
    X_WINDOW_FIXED | X_WINDOW_SKIP_TASKBAR | X_WINDOW_NO_FOCUS |
    X_WINDOW_STICKY);
XSelectInput(display, taskbar, X_MASK_BUTTON_PRESS);
XMapWindow(display, taskbar);
XSetWallpaperColor(display, XRGB(22, 30, 45));
```

Redraw its app buttons from the live window list:

```c
static void redraw_taskbar(Display *display, Window *taskbar,
                           uint32_t screen_width)
{
    Window *apps[XLIB_MAX_WINDOWS];
    uint32_t count = 0;
    if (!XListWindows(display, apps, XLIB_MAX_WINDOWS, &count))
        return;

    XClearWindow(display, taskbar);
    if (!count)
        return;
    uint32_t slot = screen_width / count;
    if (slot <= 12)
        return;
    for (uint32_t i = 0; i < count; i++)
    {
        XWindowAttributes app;
        if (!XGetWindowAttributes(display, apps[i], &app))
            continue;
        int32_t x = (int32_t)(i * slot + 6);
        XDrawRectangle(display, taskbar, XRGB(85, 100, 125),
                       x, 4, slot - 12, 32);
        XDrawString(display, taskbar, XRGB(245, 245, 250),
                    x + 6, 12, app.title);
    }
}
```

In the event loop, map a taskbar click to its slot. If the selected window is
minimized, switch to its workspace and call `XRestoreWindow`; otherwise call
`XRaiseWindow` and `XSetInputFocus`. Use `XSetCurrentWorkspace` for workspace
buttons and redraw after minimize/restore/configure/workspace events.

To write a custom window manager, turn off built-in policy with
`XWMSetFeatures(display, 0)`, select root events with
`XSelectInput(display, XDefaultRootWindow(display), ...)`, and use
`XWindowAtPosition`, `XGetWindowAttributes`, `XMoveResizeWindow`,
`XRaiseWindow`, `XLowerWindow`, `XIconifyWindow`, `XMaximizeWindow`,
`XRestoreWindow`, and `XSetInputFocus` to implement your own interactions.
Create client windows with `X_WINDOW_UNDECORATED` and draw your title bars and
controls yourself. Set root and per-window event masks for pointer, keyboard,
focus, configure, and workspace events as needed.

The default system boot currently starts the terminal directly. The desktop
shell and Plymouth launcher are not included, but libx remains available for
building a future desktop or graphical application with the APIs above.
libx is a process-local windowing model, not yet a system-wide manager for
independently running GUI processes.

For regular applications, the default manager handles common operations:

```c
Display *display = XOpenDisplay();
Window *window = XCreateSimpleWindow(
    display, XDefaultRootWindow(display),
    80, 60, 480, 320, XRGB(32, 38, 48), "My desktop app");

XSelectInput(display, window, X_MASK_ALL);
XMapWindow(display, window);
XDrawString(display, window, XRGB(255, 255, 255), 16, 16, "Hello");
XFlush(display);

XEvent event;
while (XNextEvent(display, &event))
{
    if (event.type == X_EVENT_CLOSE_REQUEST)
        break;
    XFlush(display);
}
XCloseDisplay(display);
```

Calling `XDrawString` again at the same window coordinates replaces that text
item, which lets dynamic labels be redrawn without exhausting the retained
text slots. `XClearWindow` removes the window's recorded text items.

This is not upstream Xlib, XCB, or the X11 wire protocol. Each process that
opens the display draws to the same system framebuffer and reads the shared
input stream. Window surfaces, workspaces, focus, z-order, and event routing
are local to that process. This supports a complete *single-process desktop
shell/compositor* built on `libx`, but not a multi-process X server: independent
graphical programs can overwrite each other's output, and input is not
isolated per client. The library does not provide alpha compositing, process
launching, image decoding, font rasterization, or a separate client/server
protocol. Applications can use the existing PNG and FreeType libraries to
decode wallpaper and render richer text into their window surfaces.
