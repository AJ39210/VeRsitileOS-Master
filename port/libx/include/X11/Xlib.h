#ifndef VERSITILE_LIBX_XLIB_H
#define VERSITILE_LIBX_XLIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XLIB_TITLE_MAX 96
#define XLIB_TEXT_MAX 192
#define XLIB_MAX_WINDOWS 32

typedef struct XDisplay Display;
typedef struct XWindow Window;

typedef enum
{
    X_EVENT_NONE = 0,
    X_EVENT_EXPOSE,
    X_EVENT_KEY_PRESS,
    X_EVENT_KEY_RELEASE,
    X_EVENT_BUTTON_PRESS,
    X_EVENT_BUTTON_RELEASE,
    X_EVENT_POINTER_MOTION,
    X_EVENT_FOCUS_IN,
    X_EVENT_FOCUS_OUT,
    X_EVENT_CLOSE_REQUEST,
    X_EVENT_CONFIGURE,
    X_EVENT_MINIMIZE,
    X_EVENT_RESTORE,
    X_EVENT_WORKSPACE
} XEventType;

typedef struct
{
    XEventType type;
    Window *window;
    int32_t x;
    int32_t y;
    uint32_t keycode;
    uint32_t character;
    uint32_t button;
    uint32_t buttons;
    uint32_t workspace;
    uint8_t pressed;
    uint8_t shift;
    uint8_t caps_lock;
    uint8_t extended;
    uint32_t width;
    uint32_t height;
} XEvent;

enum
{
    X_MASK_KEY_PRESS = 1U << 0,
    X_MASK_KEY_RELEASE = 1U << 1,
    X_MASK_BUTTON_PRESS = 1U << 2,
    X_MASK_BUTTON_RELEASE = 1U << 3,
    X_MASK_POINTER_MOTION = 1U << 4,
    X_MASK_FOCUS = 1U << 5,
    X_MASK_CLOSE = 1U << 6,
    X_MASK_CONFIGURE = 1U << 7,
    X_MASK_WINDOW_STATE = 1U << 8,
    X_MASK_WORKSPACE = 1U << 9,
    X_MASK_ALL = (1U << 10) - 1
};

enum
{
    X_WINDOW_UNDECORATED = 1U << 0,
    X_WINDOW_ALWAYS_ON_TOP = 1U << 1,
    X_WINDOW_FIXED = 1U << 2,
    X_WINDOW_SKIP_TASKBAR = 1U << 3,
    X_WINDOW_RESIZABLE = 1U << 4,
    X_WINDOW_NO_FOCUS = 1U << 5,
    X_WINDOW_STICKY = 1U << 6
};

typedef struct
{
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t flags;
    uint32_t workspace;
    char title[XLIB_TITLE_MAX];
    bool mapped;
    bool focused;
    bool minimized;
    bool maximized;
} XWindowAttributes;

typedef enum
{
    X_CURSOR_ARROW = 0,
    X_CURSOR_IBEAM,
    X_CURSOR_HAND,
    X_CURSOR_CROSSHAIR,
    X_CURSOR_HIDDEN
} XCursorShape;

typedef struct
{
    uint32_t red;
    uint32_t green;
    uint32_t blue;
} XColor;

static inline uint32_t XRGB(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
}

Display *XOpenDisplay(void);
void XCloseDisplay(Display *display);
Window *XDefaultRootWindow(Display *display);

Window *XCreateSimpleWindow(
    Display *display,
    Window *parent,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t background,
    const char *title);
bool XDestroyWindow(Display *display, Window *window);
bool XMapWindow(Display *display, Window *window);
bool XUnmapWindow(Display *display, Window *window);
bool XSetWindowTitle(Display *display, Window *window, const char *title);
bool XSetWindowBackground(Display *display, Window *window, uint32_t color);
bool XMoveResizeWindow(
    Display *display,
    Window *window,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height);
bool XMoveWindow(Display *display, Window *window, int32_t x, int32_t y);
bool XSetWindowFlags(Display *display, Window *window, uint32_t flags);
bool XGetWindowAttributes(
    Display *display,
    Window *window,
    XWindowAttributes *attributes);
bool XListWindows(
    Display *display,
    Window **windows,
    uint32_t capacity,
    uint32_t *count);
Window *XWindowAtPosition(Display *display, int32_t x, int32_t y);
bool XRaiseWindow(Display *display, Window *window);
bool XLowerWindow(Display *display, Window *window);
bool XIconifyWindow(Display *display, Window *window);
bool XRestoreWindow(Display *display, Window *window);
bool XMaximizeWindow(Display *display, Window *window);
bool XSetWindowMinimumSize(
    Display *display,
    Window *window,
    uint32_t minimum_width,
    uint32_t minimum_height);
bool XSelectInput(Display *display, Window *window, uint32_t event_mask);
bool XSetInputFocus(Display *display, Window *window);

bool XClearWindow(Display *display, Window *window);
bool XFillRectangle(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height);
bool XDrawRectangle(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height);
bool XDrawLine(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2);
bool XDrawString(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    const char *text);
bool XPutImage(
    Display *display,
    Window *window,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint32_t stride);

bool XPollEvent(Display *display, XEvent *event);
bool XNextEvent(Display *display, XEvent *event);
uint32_t XPending(Display *display);
bool XFlush(Display *display);
int XLastError(const Display *display);

#ifdef __cplusplus
}
#endif

#endif
