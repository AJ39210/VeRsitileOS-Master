#ifndef VERSITILE_LIBX_XDESKTOP_H
#define VERSITILE_LIBX_XDESKTOP_H

#include <stdbool.h>
#include <stdint.h>

#include <X11/Xlib.h>

#define X_MAX_WORKSPACES 16

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    X_WM_DECORATIONS = 1U << 0,
    X_WM_DRAG_WINDOWS = 1U << 1,
    X_WM_AUTO_FOCUS = 1U << 2,
    X_WM_AUTO_RAISE = 1U << 3,
    X_WM_RESIZE_WINDOWS = 1U << 4
};

typedef struct
{
    uint32_t active_title;
    uint32_t inactive_title;
    uint32_t title_text;
    uint32_t border;
    uint32_t button_background;
    uint32_t button_foreground;
    uint32_t close_background;
} XWMDecorationTheme;

bool XWMSetFeatures(Display *display, uint32_t features);
bool XSetWorkArea(
    Display *display,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height);
bool XWMSetDecorationTheme(
    Display *display,
    const XWMDecorationTheme *theme);
bool XGetScreenSize(Display *display, uint32_t *width, uint32_t *height);
bool XSetWorkspaceCount(Display *display, uint32_t count);
uint32_t XGetWorkspaceCount(Display *display);
bool XSetCurrentWorkspace(Display *display, uint32_t workspace);
uint32_t XGetCurrentWorkspace(Display *display);
bool XSetWindowWorkspace(
    Display *display,
    Window *window,
    uint32_t workspace);
bool XGetWindowWorkspace(
    Display *display,
    Window *window,
    uint32_t *workspace);
bool XSetWallpaperColor(Display *display, uint32_t color);
bool XSetWallpaperImage(
    Display *display,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint32_t stride);
bool XSetCursor(Display *display, XCursorShape shape);
bool XSetCursorImage(
    Display *display,
    uint32_t width,
    uint32_t height,
    const uint32_t *argb_pixels,
    uint32_t stride,
    uint32_t hotspot_x,
    uint32_t hotspot_y);
bool XGetPointerPosition(Display *display, int32_t *x, int32_t *y);

#ifdef __cplusplus
}
#endif

#endif
