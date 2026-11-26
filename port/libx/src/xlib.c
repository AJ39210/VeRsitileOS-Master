#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xdesktop.h>
#include <os/gui.h>

#define XLIB_EVENT_QUEUE_SIZE 128
#define XLIB_TEXT_ITEMS 64
#define XLIB_WINDOW_BYTES_MAX (16U * 1024U * 1024U)
#define XLIB_SURFACE_BYTES_MAX (64U * 1024U * 1024U)
#define XLIB_CURSOR_BYTES_MAX (64U * 64U * sizeof(uint32_t))
#define XLIB_CURSOR_PIXEL_MAX 64U
#define XLIB_TITLE_HEIGHT 24
#define XLIB_BORDER_SIZE 2
#define XLIB_RESIZE_BORDER 6
#define XLIB_DRAW_BATCH_SIZE 16
#define XLIB_WINDOW_FLAGS_ALL \
    (X_WINDOW_UNDECORATED | X_WINDOW_ALWAYS_ON_TOP | X_WINDOW_FIXED | \
     X_WINDOW_SKIP_TASKBAR | X_WINDOW_RESIZABLE | X_WINDOW_NO_FOCUS | \
     X_WINDOW_STICKY)
#define XLIB_WM_FEATURES_ALL \
    (X_WM_DECORATIONS | X_WM_DRAG_WINDOWS | X_WM_AUTO_FOCUS | \
     X_WM_AUTO_RAISE | X_WM_RESIZE_WINDOWS)

typedef struct
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} xlib_damage_t;

typedef struct
{
    int32_t x;
    int32_t y;
    uint32_t color;
    char text[XLIB_TEXT_MAX];
} xlib_text_item_t;

struct XWindow
{
    Display *display;
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t background;
    uint32_t event_mask;
    uint32_t flags;
    uint32_t workspace;
    uint32_t minimum_width;
    uint32_t minimum_height;
    int32_t x;
    int32_t y;
    int32_t restore_x;
    int32_t restore_y;
    uint32_t restore_width;
    uint32_t restore_height;
    char title[XLIB_TITLE_MAX];
    xlib_text_item_t text[XLIB_TEXT_ITEMS];
    uint32_t text_count;
    bool mapped;
    bool focused;
    bool minimized;
    bool maximized;
};

struct XDisplay
{
    os_screen_info_t screen;
    Window root;
    Window *windows[XLIB_MAX_WINDOWS];
    uint32_t window_count;
    uint32_t *canvas;
    uint32_t wallpaper_color;
    uint32_t *wallpaper_pixels;
    uint32_t wallpaper_width;
    uint32_t wallpaper_height;
    uint32_t wallpaper_stride;
    uint32_t *cursor_pixels;
    uint32_t cursor_width;
    uint32_t cursor_height;
    uint32_t cursor_hotspot_x;
    uint32_t cursor_hotspot_y;
    uint32_t cursor_underlay[XLIB_CURSOR_PIXEL_MAX * XLIB_CURSOR_PIXEL_MAX];
    uint32_t cursor_underlay_x;
    uint32_t cursor_underlay_y;
    uint32_t cursor_underlay_width;
    uint32_t cursor_underlay_height;
    bool cursor_underlay_valid;
    bool scene_dirty;
    bool cursor_dirty;
    bool damage_valid;
    xlib_damage_t damage;
    uint32_t wm_features;
    XWMDecorationTheme decoration_theme;
    uint32_t current_workspace;
    uint32_t workspace_count;
    uint32_t workarea_x;
    uint32_t workarea_y;
    uint32_t workarea_width;
    uint32_t workarea_height;
    uint64_t allocated_surface_bytes;
    XCursorShape cursor_shape;
    int32_t pointer_x;
    int32_t pointer_y;
    int64_t drag_offset_x;
    int64_t drag_offset_y;
    int32_t drag_start_x;
    int32_t drag_start_y;
    int32_t drag_window_x;
    int32_t drag_window_y;
    uint32_t drag_window_width;
    uint32_t drag_window_height;
    uint32_t resize_edges;
    Window *focused;
    Window *dragged;
    uint32_t previous_buttons;
    XEvent events[XLIB_EVENT_QUEUE_SIZE];
    uint32_t event_read;
    uint32_t event_count;
    int last_error;
};

static bool window_belongs_to(Display *display, Window *window);
static uint32_t window_frame_height(
    const Display *display,
    const Window *window);

static void mark_damage(
    Display *display,
    int64_t x,
    int64_t y,
    uint64_t width,
    uint64_t height)
{
    if (!display || !width || !height)
        return;
    int64_t right = x + (int64_t)width;
    int64_t bottom = y + (int64_t)height;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (right > (int64_t)display->screen.width)
        right = (int64_t)display->screen.width;
    if (bottom > (int64_t)display->screen.height)
        bottom = (int64_t)display->screen.height;
    if (right <= x || bottom <= y)
        return;

    uint32_t left = (uint32_t)x;
    uint32_t top = (uint32_t)y;
    uint32_t clipped_right = (uint32_t)right;
    uint32_t clipped_bottom = (uint32_t)bottom;
    if (!display->damage_valid)
    {
        display->damage = (xlib_damage_t){
            .x = left,
            .y = top,
            .width = clipped_right - left,
            .height = clipped_bottom - top
        };
        display->damage_valid = true;
        return;
    }

    uint32_t damage_right =
        display->damage.x + display->damage.width;
    uint32_t damage_bottom =
        display->damage.y + display->damage.height;
    if (left < display->damage.x)
        display->damage.x = left;
    if (top < display->damage.y)
        display->damage.y = top;
    if (clipped_right > damage_right)
        damage_right = clipped_right;
    if (clipped_bottom > damage_bottom)
        damage_bottom = clipped_bottom;
    display->damage.width = damage_right - display->damage.x;
    display->damage.height = damage_bottom - display->damage.y;
}

static void mark_window_damage(
    Display *display,
    Window *window,
    int64_t x,
    int64_t y,
    uint64_t width,
    uint64_t height)
{
    if (!window_belongs_to(display, window))
        return;
    mark_damage(
        display,
        (int64_t)window->x + x,
        (int64_t)window->y +
            window_frame_height(display, window) + y,
        width,
        height);
}

static bool window_belongs_to(Display *display, Window *window)
{
    if (!display || !window || window == &display->root ||
        window->display != display)
        return false;
    for (uint32_t i = 0; i < display->window_count; i++)
    {
        if (display->windows[i] == window)
            return true;
    }
    return false;
}

static Window *top_mapped_window(Display *display);

static bool dimensions_valid(
    uint32_t width,
    uint32_t height,
    uint32_t bytes_per_pixel)
{
    return width && height &&
        width <= UINT32_MAX / bytes_per_pixel &&
        height <= UINT32_MAX / (width * bytes_per_pixel);
}

static bool image_stride_valid(uint32_t height, uint32_t stride)
{
    return height && stride &&
        (uint64_t)(height - 1) <=
            (uint64_t)SIZE_MAX / sizeof(uint32_t) / stride;
}

static bool queue_event(
    Display *display,
    Window *window,
    XEvent event,
    uint32_t mask)
{
    if (!window || !(window->event_mask & mask))
        return true;
    if (display->event_count == XLIB_EVENT_QUEUE_SIZE)
    {
        display->last_error = EOVERFLOW;
        return false;
    }
    event.window = window;
    uint32_t index =
        (display->event_read + display->event_count) %
        XLIB_EVENT_QUEUE_SIZE;
    display->events[index] = event;
    display->event_count++;
    return true;
}

static uint32_t window_frame_height(
    const Display *display,
    const Window *window)
{
    return (display->wm_features & X_WM_DECORATIONS) &&
        !(window->flags & X_WINDOW_UNDECORATED)
        ? XLIB_TITLE_HEIGHT
        : 0;
}

static void set_focus(Display *display, Window *window)
{
    if (display->focused == window)
        return;
    display->scene_dirty = true;
    if (display->focused)
    {
        display->focused->focused = false;
        (void)queue_event(
            display, display->focused,
            (XEvent){ .type = X_EVENT_FOCUS_OUT }, X_MASK_FOCUS);
    }
    display->focused = window;
    if (window)
    {
        window->focused = true;
        (void)queue_event(
            display, window,
            (XEvent){ .type = X_EVENT_FOCUS_IN }, X_MASK_FOCUS);
    }
}

static void raise_window(Display *display, Window *window)
{
    uint32_t index = 0;
    while (index < display->window_count &&
           display->windows[index] != window)
        index++;
    if (index == display->window_count)
        return;
    if (index + 1 == display->window_count)
        return;
    display->scene_dirty = true;
    uint32_t destination = display->window_count - 1;
    if (!(window->flags & X_WINDOW_ALWAYS_ON_TOP))
    {
        while (destination > 0 &&
               (display->windows[destination]->flags &
                X_WINDOW_ALWAYS_ON_TOP))
            destination--;
        if (destination < index)
            destination = index;
    }
    for (uint32_t i = index; i < destination; i++)
        display->windows[i] = display->windows[i + 1];
    display->windows[destination] = window;
}

static void lower_window(Display *display, Window *window)
{
    uint32_t index = 0;
    while (index < display->window_count &&
           display->windows[index] != window)
        index++;
    if (index == display->window_count || !index)
        return;
    display->scene_dirty = true;
    uint32_t destination = 0;
    if (window->flags & X_WINDOW_ALWAYS_ON_TOP)
    {
        while (destination < index &&
               !(display->windows[destination]->flags &
                 X_WINDOW_ALWAYS_ON_TOP))
            destination++;
    }
    for (uint32_t i = index; i > destination; i--)
        display->windows[i] = display->windows[i - 1];
    display->windows[destination] = window;
}

static void restack_window(Display *display, Window *window)
{
    if (window->flags & X_WINDOW_ALWAYS_ON_TOP)
        raise_window(display, window);
    else
        lower_window(display, window);
}

static bool copy_string(char *destination, size_t capacity, const char *source)
{
    if (!destination || !capacity || !source)
        return false;
    size_t i = 0;
    while (i + 1 < capacity && source[i])
    {
        destination[i] = source[i];
        i++;
    }
    if (source[i])
        return false;
    destination[i] = '\0';
    return true;
}

static int32_t clamp_int32(int64_t value)
{
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}

Display *XOpenDisplay(void)
{
    Display *display = calloc(1, sizeof(*display));
    if (!display)
        return NULL;
    if (os_screen_info(&display->screen) < 0 ||
        display->screen.width > UINT32_MAX ||
        display->screen.height > UINT32_MAX ||
        !dimensions_valid(
            (uint32_t)display->screen.width,
            (uint32_t)display->screen.height,
            sizeof(uint32_t)))
    {
        free(display);
        return NULL;
    }
    uint64_t pixel_count =
        display->screen.width * display->screen.height;
    if (pixel_count > XLIB_SURFACE_BYTES_MAX / sizeof(uint32_t))
    {
        free(display);
        return NULL;
    }
    display->canvas = calloc((size_t)pixel_count, sizeof(uint32_t));
    if (!display->canvas)
    {
        free(display);
        return NULL;
    }
    display->root.display = display;
    display->root.width = (uint32_t)display->screen.width;
    display->root.height = (uint32_t)display->screen.height;
    display->workarea_width = (uint32_t)display->screen.width;
    display->workarea_height = (uint32_t)display->screen.height;
    display->root.mapped = true;
    display->root.event_mask = X_MASK_ALL;
    display->wallpaper_color = XRGB(24, 34, 52);
    display->wm_features = X_WM_DECORATIONS | X_WM_DRAG_WINDOWS |
        X_WM_AUTO_FOCUS | X_WM_AUTO_RAISE | X_WM_RESIZE_WINDOWS;
    display->decoration_theme = (XWMDecorationTheme){
        .active_title = XRGB(42, 94, 168),
        .inactive_title = XRGB(64, 70, 80),
        .title_text = XRGB(255, 255, 255),
        .border = XRGB(20, 24, 30),
        .button_background = XRGB(52, 94, 132),
        .button_foreground = XRGB(245, 245, 250),
        .close_background = XRGB(198, 70, 70)
    };
    display->workspace_count = 4;
    display->cursor_shape = X_CURSOR_ARROW;
    display->pointer_x = (int32_t)(display->screen.width / 2);
    display->pointer_y = (int32_t)(display->screen.height / 2);
    display->scene_dirty = true;
    display->cursor_dirty = true;
    return display;
}

void XCloseDisplay(Display *display)
{
    if (!display)
        return;
    for (uint32_t i = 0; i < display->window_count; i++)
    {
        free(display->windows[i]->pixels);
        free(display->windows[i]);
    }
    free(display->wallpaper_pixels);
    free(display->cursor_pixels);
    free(display->canvas);
    free(display);
}

Window *XDefaultRootWindow(Display *display)
{
    return display ? &display->root : NULL;
}

Window *XCreateSimpleWindow(
    Display *display,
    Window *parent,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t background,
    const char *title)
{
    if (!display || (parent && parent != &display->root) ||
        display->window_count == XLIB_MAX_WINDOWS ||
        !dimensions_valid(width, height, sizeof(uint32_t)) ||
        (uint64_t)width * height * sizeof(uint32_t) >
            XLIB_WINDOW_BYTES_MAX)
    {
        if (display)
            display->last_error = EINVAL;
        return NULL;
    }
    uint64_t surface_bytes =
        (uint64_t)width * height * sizeof(uint32_t);
    if (surface_bytes >
        XLIB_SURFACE_BYTES_MAX - display->allocated_surface_bytes)
    {
        display->last_error = ENOMEM;
        return NULL;
    }

    Window *window = calloc(1, sizeof(*window));
    if (!window)
    {
        display->last_error = ENOMEM;
        return NULL;
    }
    window->pixels = malloc((size_t)width * height * sizeof(uint32_t));
    if (!window->pixels)
    {
        free(window);
        display->last_error = ENOMEM;
        return NULL;
    }
    window->display = display;
    window->width = width;
    window->height = height;
    window->background = background;
    window->event_mask = X_MASK_ALL;
    window->flags = X_WINDOW_RESIZABLE;
    window->workspace = display->current_workspace;
    window->minimum_width = 1;
    window->minimum_height = 1;
    window->x = x;
    window->y = y;
    if (!copy_string(window->title, sizeof(window->title),
                     title ? title : "Window"))
    {
        free(window->pixels);
        free(window);
        display->last_error = ENAMETOOLONG;
        return NULL;
    }
    for (size_t i = 0; i < (size_t)width * height; i++)
        window->pixels[i] = background;
    display->windows[display->window_count++] = window;
    display->allocated_surface_bytes += surface_bytes;
    return window;
}

bool XDestroyWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    bool had_focus = display->focused == window;
    if (had_focus)
        set_focus(display, NULL);
    if (display->dragged == window)
    {
        display->dragged = NULL;
        display->resize_edges = 0;
    }
    uint32_t index = 0;
    while (display->windows[index] != window)
        index++;
    for (uint32_t i = index; i + 1 < display->window_count; i++)
        display->windows[i] = display->windows[i + 1];
    display->window_count--;
    display->scene_dirty = true;
    display->allocated_surface_bytes -=
        (uint64_t)window->width * window->height * sizeof(uint32_t);
    free(window->pixels);
    free(window);
    if (had_focus)
        set_focus(display, top_mapped_window(display));
    return true;
}

bool XMapWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    window->mapped = true;
    window->minimized = false;
    display->scene_dirty = true;
    raise_window(display, window);
    (void)queue_event(
        display, window,
        (XEvent){ .type = X_EVENT_EXPOSE }, X_MASK_ALL);
    return true;
}

bool XUnmapWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    window->mapped = false;
    window->minimized = false;
    display->scene_dirty = true;
    if (display->dragged == window)
    {
        display->dragged = NULL;
        display->resize_edges = 0;
    }
    bool had_focus = display->focused == window;
    if (had_focus)
    {
        set_focus(display, NULL);
        set_focus(display, top_mapped_window(display));
    }
    return true;
}

bool XSetWindowTitle(Display *display, Window *window, const char *title)
{
    if (!window_belongs_to(display, window) ||
        !copy_string(window->title, sizeof(window->title), title))
    {
        if (display)
            display->last_error = ENAMETOOLONG;
        return false;
    }
    display->scene_dirty = true;
    return true;
}

bool XSetWindowBackground(Display *display, Window *window, uint32_t color)
{
    if (!window_belongs_to(display, window))
        return false;
    window->background = color;
    return XClearWindow(display, window);
}

bool XMoveResizeWindow(
    Display *display,
    Window *window,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height)
{
    if (!window_belongs_to(display, window) ||
        !dimensions_valid(width, height, sizeof(uint32_t)) ||
        width < window->minimum_width ||
        height < window->minimum_height ||
        (uint64_t)width * height * sizeof(uint32_t) >
            XLIB_WINDOW_BYTES_MAX)
    {
        if (display)
            display->last_error = EINVAL;
        return false;
    }
    if (width != window->width || height != window->height)
    {
        uint64_t old_bytes =
            (uint64_t)window->width * window->height * sizeof(uint32_t);
        uint64_t new_bytes = (uint64_t)width * height * sizeof(uint32_t);
        if (new_bytes > old_bytes &&
            new_bytes - old_bytes >
                XLIB_SURFACE_BYTES_MAX - display->allocated_surface_bytes)
        {
            display->last_error = ENOMEM;
            return false;
        }
        uint32_t *pixels = malloc((size_t)width * height * sizeof(uint32_t));
        if (!pixels)
        {
            display->last_error = ENOMEM;
            return false;
        }
        for (size_t i = 0; i < (size_t)width * height; i++)
            pixels[i] = window->background;
        uint32_t copy_width =
            width < window->width ? width : window->width;
        uint32_t copy_height =
            height < window->height ? height : window->height;
        for (uint32_t row = 0; row < copy_height; row++)
            memcpy(
                pixels + (size_t)row * width,
                window->pixels + (size_t)row * window->width,
                (size_t)copy_width * sizeof(uint32_t));
        free(window->pixels);
        window->pixels = pixels;
        window->width = width;
        window->height = height;
        window->text_count = 0;
        display->allocated_surface_bytes =
            display->allocated_surface_bytes - old_bytes + new_bytes;
    }
    window->x = x;
    window->y = y;
    display->scene_dirty = true;
    (void)queue_event(
        display, window,
        (XEvent){
            .type = X_EVENT_CONFIGURE,
            .x = x,
            .y = y,
            .width = width,
            .height = height
        },
        X_MASK_CONFIGURE);
    return true;
}

bool XMoveWindow(Display *display, Window *window, int32_t x, int32_t y)
{
    if (!window_belongs_to(display, window) ||
        (window->flags & X_WINDOW_FIXED))
        return false;
    window->x = x;
    window->y = y;
    display->scene_dirty = true;
    (void)queue_event(
        display, window,
        (XEvent){
            .type = X_EVENT_CONFIGURE,
            .x = x,
            .y = y,
            .width = window->width,
            .height = window->height
        },
        X_MASK_CONFIGURE);
    return true;
}

bool XSetWindowFlags(Display *display, Window *window, uint32_t flags)
{
    if (!window_belongs_to(display, window) ||
        (flags & ~XLIB_WINDOW_FLAGS_ALL))
        return false;
    bool was_always_on_top =
        (window->flags & X_WINDOW_ALWAYS_ON_TOP) != 0;
    window->flags = flags;
    display->scene_dirty = true;
    if (was_always_on_top !=
        ((flags & X_WINDOW_ALWAYS_ON_TOP) != 0))
        restack_window(display, window);
    (void)queue_event(
        display, window,
        (XEvent){
            .type = X_EVENT_CONFIGURE,
            .x = window->x,
            .y = window->y,
            .width = window->width,
            .height = window->height
        },
        X_MASK_CONFIGURE);
    return true;
}

bool XGetWindowAttributes(
    Display *display,
    Window *window,
    XWindowAttributes *attributes)
{
    if (!window_belongs_to(display, window) || !attributes)
        return false;
    *attributes = (XWindowAttributes){
        .x = window->x,
        .y = window->y,
        .width = window->width,
        .height = window->height,
        .flags = window->flags,
        .workspace = window->workspace,
        .mapped = window->mapped,
        .focused = window->focused,
        .minimized = window->minimized,
        .maximized = window->maximized
    };
    memcpy(attributes->title, window->title, sizeof(attributes->title));
    return true;
}

bool XListWindows(
    Display *display,
    Window **windows,
    uint32_t capacity,
    uint32_t *count)
{
    if (!display || !count || (capacity && !windows))
        return false;
    uint32_t total = 0;
    for (uint32_t i = 0; i < display->window_count; i++)
    {
        Window *window = display->windows[i];
        if (window->flags & X_WINDOW_SKIP_TASKBAR)
            continue;
        if (total < capacity)
            windows[total] = window;
        total++;
    }
    *count = total;
    return total <= capacity;
}

static bool window_visible_at(
    const Display *display,
    const Window *window,
    int32_t x,
    int32_t y)
{
    uint32_t frame = window_frame_height(display, window);
    return window->mapped &&
        ((window->flags & X_WINDOW_STICKY) ||
         window->workspace == display->current_workspace) &&
        x >= window->x &&
        (int64_t)x < (int64_t)window->x + window->width &&
        y >= window->y &&
        (int64_t)y <
            (int64_t)window->y + frame + window->height;
}

Window *XWindowAtPosition(Display *display, int32_t x, int32_t y)
{
    if (!display || x < 0 || y < 0 ||
        (uint64_t)x >= display->screen.width ||
        (uint64_t)y >= display->screen.height)
        return NULL;
    for (uint32_t i = display->window_count; i > 0; i--)
    {
        Window *window = display->windows[i - 1];
        if (window_visible_at(display, window, x, y))
            return window;
    }
    return &display->root;
}

bool XRaiseWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    raise_window(display, window);
    display->scene_dirty = true;
    return true;
}

bool XLowerWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    lower_window(display, window);
    display->scene_dirty = true;
    return true;
}

static Window *top_mapped_window(Display *display)
{
    for (uint32_t i = display->window_count; i > 0; i--)
        if (display->windows[i - 1]->mapped &&
            ((display->windows[i - 1]->flags & X_WINDOW_STICKY) ||
             display->windows[i - 1]->workspace ==
                 display->current_workspace) &&
            !(display->windows[i - 1]->flags & X_WINDOW_NO_FOCUS))
            return display->windows[i - 1];
    return &display->root;
}

bool XIconifyWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window) || !window->mapped)
        return false;
    window->mapped = false;
    window->minimized = true;
    window->focused = false;
    display->scene_dirty = true;
    if (display->dragged == window)
    {
        display->dragged = NULL;
        display->resize_edges = 0;
    }
    if (display->focused == window)
    {
        display->focused = NULL;
        (void)queue_event(
            display, window,
            (XEvent){ .type = X_EVENT_FOCUS_OUT }, X_MASK_FOCUS);
        set_focus(display, top_mapped_window(display));
    }
    (void)queue_event(
        display, window,
        (XEvent){ .type = X_EVENT_MINIMIZE }, X_MASK_WINDOW_STATE);
    return true;
}

bool XRestoreWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    window->minimized = false;
    window->mapped = true;
    display->scene_dirty = true;
    raise_window(display, window);
    set_focus(display, window);
    (void)queue_event(
        display, window,
        (XEvent){ .type = X_EVENT_RESTORE }, X_MASK_WINDOW_STATE);
    (void)queue_event(
        display, window,
        (XEvent){ .type = X_EVENT_EXPOSE }, X_MASK_ALL);
    return true;
}

bool XMaximizeWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    if (window->maximized)
    {
        if (!XMoveResizeWindow(
                display, window,
                window->restore_x,
                window->restore_y,
                window->restore_width,
                window->restore_height))
            return false;
        window->maximized = false;
        return true;
    }
    if (window->flags & X_WINDOW_FIXED)
        return false;
    uint32_t frame = window_frame_height(display, window);
    if (display->workarea_height <= frame)
        return false;
    window->restore_x = window->x;
    window->restore_y = window->y;
    window->restore_width = window->width;
    window->restore_height = window->height;
    window->maximized = true;
    window->minimized = false;
    window->mapped = true;
    if (!XMoveResizeWindow(
                display, window,
                (int32_t)display->workarea_x,
                (int32_t)display->workarea_y,
                display->workarea_width,
                display->workarea_height -
                window_frame_height(display, window)))
    {
        window->maximized = false;
        return false;
    }
    window->maximized = true;
    raise_window(display, window);
    set_focus(display, window);
    return true;
}

bool XSetWindowMinimumSize(
    Display *display,
    Window *window,
    uint32_t minimum_width,
    uint32_t minimum_height)
{
    if (!window_belongs_to(display, window) ||
        !minimum_width || !minimum_height ||
        minimum_width > window->width ||
        minimum_height > window->height)
        return false;
    window->minimum_width = minimum_width;
    window->minimum_height = minimum_height;
    window->flags |= X_WINDOW_RESIZABLE;
    return true;
}

bool XSelectInput(Display *display, Window *window, uint32_t event_mask)
{
    if (!display || !window ||
        (window != &display->root &&
         !window_belongs_to(display, window)) ||
        (event_mask & ~X_MASK_ALL))
        return false;
    window->event_mask = event_mask;
    return true;
}

bool XSetInputFocus(Display *display, Window *window)
{
    if (!display || !window || (window != &display->root &&
        (!window_belongs_to(display, window) || !window->mapped)))
        return false;
    if (window != &display->root)
        raise_window(display, window);
    set_focus(display, window);
    return true;
}

static Window *window_at(Display *display, int32_t x, int32_t y)
{
    Window *window = XWindowAtPosition(display, x, y);
    return window == &display->root ? NULL : window;
}

bool XClearWindow(Display *display, Window *window)
{
    if (!window_belongs_to(display, window))
        return false;
    for (size_t i = 0; i < (size_t)window->width * window->height; i++)
        window->pixels[i] = window->background;
    window->text_count = 0;
    uint32_t frame = window_frame_height(display, window);
    mark_window_damage(
        display, window, 0, -(int64_t)frame, window->width,
        (uint64_t)window->height + frame);
    return true;
}

static void set_pixel(Window *window, int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 ||
        (uint32_t)x >= window->width ||
        (uint32_t)y >= window->height)
        return;
    window->pixels[(size_t)y * window->width + (uint32_t)x] = color;
}

bool XFillRectangle(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height)
{
    if (!window_belongs_to(display, window))
        return false;
    if (!width || !height)
        return true;
    int64_t x_end = (int64_t)x + width;
    int64_t y_end = (int64_t)y + height;
    int32_t left = x < 0 ? 0 : x;
    int32_t top = y < 0 ? 0 : y;
    int32_t right = x_end > window->width
        ? (int32_t)window->width : (int32_t)x_end;
    int32_t bottom = y_end > window->height
        ? (int32_t)window->height : (int32_t)y_end;
    for (int32_t row = top; row < bottom; row++)
        for (int32_t column = left; column < right; column++)
            set_pixel(window, column, row, color);
    if (right > left && bottom > top)
        mark_window_damage(
            display, window, left, top,
            (uint64_t)(right - left), (uint64_t)(bottom - top));
    return true;
}

bool XDrawRectangle(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height)
{
    if (!window_belongs_to(display, window))
        return false;
    if (!width || !height)
        return true;
    for (uint32_t i = 0; i < width; i++)
    {
        int64_t px = (int64_t)x + i;
        int64_t bottom = (int64_t)y + height - 1;
        if (px >= INT32_MIN && px <= INT32_MAX)
        {
            set_pixel(window, (int32_t)px, y, color);
            if (bottom >= INT32_MIN && bottom <= INT32_MAX)
                set_pixel(window, (int32_t)px, (int32_t)bottom, color);
        }
    }
    for (uint32_t i = 0; i < height; i++)
    {
        int64_t py = (int64_t)y + i;
        int64_t right = (int64_t)x + width - 1;
        if (py >= INT32_MIN && py <= INT32_MAX)
        {
            set_pixel(window, x, (int32_t)py, color);
            if (right >= INT32_MIN && right <= INT32_MAX)
                set_pixel(window, (int32_t)right, (int32_t)py, color);
        }
    }
    mark_window_damage(display, window, x, y, width, height);
    return true;
}

static uint8_t line_outcode(const Window *window, int64_t x, int64_t y)
{
    uint8_t code = 0;
    if (x < 0)
        code |= 1;
    else if (x >= window->width)
        code |= 2;
    if (y < 0)
        code |= 4;
    else if (y >= window->height)
        code |= 8;
    return code;
}

static bool clip_line(
    const Window *window,
    int32_t *x1,
    int32_t *y1,
    int32_t *x2,
    int32_t *y2)
{
    int64_t left_x = *x1;
    int64_t left_y = *y1;
    int64_t right_x = *x2;
    int64_t right_y = *y2;
    int64_t limit =
        ((int64_t)window->width + window->height + 1) * 2;
    if (left_x < -limit || left_x > limit ||
        right_x < -limit || right_x > limit ||
        left_y < -limit || left_y > limit ||
        right_y < -limit || right_y > limit)
        return false;

    for (uint32_t attempt = 0; attempt < 8; attempt++)
    {
        uint8_t first = line_outcode(window, left_x, left_y);
        uint8_t second = line_outcode(window, right_x, right_y);
        if (!(first | second))
        {
            *x1 = (int32_t)left_x;
            *y1 = (int32_t)left_y;
            *x2 = (int32_t)right_x;
            *y2 = (int32_t)right_y;
            return true;
        }
        if (first & second)
            return false;

        uint8_t outside = first ? first : second;
        int64_t x = 0;
        int64_t y = 0;
        int64_t dx = right_x - left_x;
        int64_t dy = right_y - left_y;
        if (outside & 8)
        {
            y = window->height - 1;
            x = left_x + dx * (y - left_y) / dy;
        }
        else if (outside & 4)
        {
            y = 0;
            x = left_x + dx * (y - left_y) / dy;
        }
        else if (outside & 2)
        {
            x = window->width - 1;
            y = left_y + dy * (x - left_x) / dx;
        }
        else
        {
            x = 0;
            y = left_y + dy * (x - left_x) / dx;
        }
        if (outside == first)
        {
            left_x = x;
            left_y = y;
        }
        else
        {
            right_x = x;
            right_y = y;
        }
    }
    return false;
}

bool XDrawLine(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2)
{
    if (!window_belongs_to(display, window))
        return false;
    if (!clip_line(window, &x1, &y1, &x2, &y2))
        return true;
    int64_t dx = x2 >= x1 ? (int64_t)x2 - x1 : (int64_t)x1 - x2;
    int64_t dy = y2 >= y1 ? (int64_t)y2 - y1 : (int64_t)y1 - y2;
    int32_t sx = x1 < x2 ? 1 : -1;
    int32_t sy = y1 < y2 ? 1 : -1;
    int64_t error = dx - dy;
    int32_t clipped_x1 = x1;
    int32_t clipped_y1 = y1;
    int32_t clipped_x2 = x2;
    int32_t clipped_y2 = y2;
    for (;;)
    {
        set_pixel(window, x1, y1, color);
        if (x1 == x2 && y1 == y2)
            break;
        int64_t twice_error = error * 2;
        if (twice_error > -dy)
        {
            error -= dy;
            x1 += sx;
        }
        if (twice_error < dx)
        {
            error += dx;
            y1 += sy;
        }
    }
    int32_t left = clipped_x1 < clipped_x2 ? clipped_x1 : clipped_x2;
    int32_t top = clipped_y1 < clipped_y2 ? clipped_y1 : clipped_y2;
    int32_t right = clipped_x1 > clipped_x2 ? clipped_x1 : clipped_x2;
    int32_t bottom = clipped_y1 > clipped_y2 ? clipped_y1 : clipped_y2;
    mark_window_damage(
        display, window, left, top,
        (uint64_t)((int64_t)right - left + 1),
        (uint64_t)((int64_t)bottom - top + 1));
    return true;
}

bool XDrawString(
    Display *display,
    Window *window,
    uint32_t color,
    int32_t x,
    int32_t y,
    const char *text)
{
    if (!window_belongs_to(display, window) || !text)
        return false;
    for (uint32_t i = 0; i < window->text_count; i++)
    {
        xlib_text_item_t *item = &window->text[i];
        if (item->x != x || item->y != y)
            continue;
        size_t old_length = strlen(item->text);
        if (!copy_string(item->text, sizeof(item->text), text))
        {
            display->last_error = ENAMETOOLONG;
            return false;
        }
        item->color = color;
        size_t new_length = strlen(item->text);
        size_t damage_length =
            old_length > new_length ? old_length : new_length;
        mark_window_damage(
            display, window, x, y, damage_length * 8, 16);
        return true;
    }
    if (window->text_count == XLIB_TEXT_ITEMS)
    {
        display->last_error = ENOSPC;
        return false;
    }
    xlib_text_item_t *item = &window->text[window->text_count];
    if (!copy_string(item->text, sizeof(item->text), text))
    {
        display->last_error = ENAMETOOLONG;
        return false;
    }
    item->x = x;
    item->y = y;
    item->color = color;
    window->text_count++;
    mark_window_damage(
        display, window, x, y, strlen(item->text) * 8, 16);
    return true;
}

bool XPutImage(
    Display *display,
    Window *window,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint32_t stride)
{
    if (!window_belongs_to(display, window) || !pixels ||
        stride < width || !image_stride_valid(height, stride) ||
        !dimensions_valid(width, height, sizeof(uint32_t)))
        return false;
    for (uint32_t row = 0; row < height; row++)
    {
        int64_t target_y = (int64_t)y + row;
        if (target_y < 0 || target_y >= window->height)
            continue;
        for (uint32_t column = 0; column < width; column++)
        {
            int64_t target_x = (int64_t)x + column;
            if (target_x < 0 || target_x >= window->width)
                continue;
            window->pixels[(size_t)target_y * window->width +
                           (size_t)target_x] =
                pixels[(size_t)row * stride + column];
        }
    }
    mark_window_damage(display, window, x, y, width, height);
    return true;
}

bool XSetWallpaperColor(Display *display, uint32_t color)
{
    if (!display)
        return false;
    free(display->wallpaper_pixels);
    display->wallpaper_pixels = NULL;
    display->wallpaper_width = 0;
    display->wallpaper_height = 0;
    display->wallpaper_color = color;
    display->scene_dirty = true;
    return true;
}

bool XSetWallpaperImage(
    Display *display,
    uint32_t width,
    uint32_t height,
    const uint32_t *pixels,
    uint32_t stride)
{
    if (!display || !pixels || stride < width ||
        !image_stride_valid(height, stride) ||
        !dimensions_valid(width, height, sizeof(uint32_t)) ||
        (uint64_t)width * height * sizeof(uint32_t) >
            XLIB_SURFACE_BYTES_MAX)
        return false;
    uint32_t *copy = malloc((size_t)width * height * sizeof(uint32_t));
    if (!copy)
    {
        display->last_error = ENOMEM;
        return false;
    }
    for (uint32_t row = 0; row < height; row++)
        memcpy(
            copy + (size_t)row * width,
            pixels + (size_t)row * stride,
            (size_t)width * sizeof(uint32_t));
    free(display->wallpaper_pixels);
    display->wallpaper_pixels = copy;
    display->wallpaper_width = width;
    display->wallpaper_height = height;
    display->wallpaper_stride = width;
    display->scene_dirty = true;
    return true;
}

bool XWMSetFeatures(Display *display, uint32_t features)
{
    if (!display ||
        (features & ~XLIB_WM_FEATURES_ALL))
        return false;
    display->wm_features = features;
    display->dragged = NULL;
    display->resize_edges = 0;
    display->scene_dirty = true;
    return true;
}

bool XSetWorkArea(
    Display *display,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height)
{
    if (!display || !width || !height ||
        (uint64_t)x + width > display->screen.width ||
        (uint64_t)y + height > display->screen.height)
    {
        if (display)
            display->last_error = EINVAL;
        return false;
    }
    display->workarea_x = x;
    display->workarea_y = y;
    display->workarea_width = width;
    display->workarea_height = height;
    return true;
}

bool XWMSetDecorationTheme(
    Display *display,
    const XWMDecorationTheme *theme)
{
    if (!display || !theme)
        return false;
    display->decoration_theme = *theme;
    display->scene_dirty = true;
    return true;
}

bool XGetScreenSize(Display *display, uint32_t *width, uint32_t *height)
{
    if (!display || !width || !height)
        return false;
    *width = (uint32_t)display->screen.width;
    *height = (uint32_t)display->screen.height;
    return true;
}

bool XSetWorkspaceCount(Display *display, uint32_t count)
{
    if (!display || !count || count > X_MAX_WORKSPACES)
        return false;
    display->workspace_count = count;
    display->scene_dirty = true;
    if (display->current_workspace >= count)
        display->current_workspace = count - 1;
    for (uint32_t i = 0; i < display->window_count; i++)
        if (display->windows[i]->workspace >= count)
            display->windows[i]->workspace = count - 1;
    if (display->focused != &display->root &&
        display->focused &&
        !(display->focused->flags & X_WINDOW_STICKY) &&
        display->focused->workspace != display->current_workspace)
        set_focus(display, &display->root);
    (void)queue_event(
        display, &display->root,
        (XEvent){
            .type = X_EVENT_WORKSPACE,
            .workspace = display->current_workspace
        },
        X_MASK_WORKSPACE);
    return true;
}

uint32_t XGetWorkspaceCount(Display *display)
{
    return display ? display->workspace_count : 0;
}

bool XSetCurrentWorkspace(Display *display, uint32_t workspace)
{
    if (!display || workspace >= display->workspace_count)
        return false;
    if (display->current_workspace == workspace)
        return true;
    display->current_workspace = workspace;
    display->scene_dirty = true;
    Window *focus = top_mapped_window(display);
    set_focus(display, focus);
    (void)queue_event(
        display, &display->root,
        (XEvent){
            .type = X_EVENT_WORKSPACE,
            .workspace = workspace
        },
        X_MASK_WORKSPACE);
    return true;
}

uint32_t XGetCurrentWorkspace(Display *display)
{
    return display ? display->current_workspace : 0;
}

bool XSetWindowWorkspace(
    Display *display,
    Window *window,
    uint32_t workspace)
{
    if (!window_belongs_to(display, window) ||
        workspace >= display->workspace_count)
        return false;
    window->workspace = workspace;
    display->scene_dirty = true;
    if (display->focused == window &&
        !(window->flags & X_WINDOW_STICKY) &&
        window->workspace != display->current_workspace)
        set_focus(display, top_mapped_window(display));
    if (window->workspace == display->current_workspace ||
        (window->flags & X_WINDOW_STICKY))
        (void)queue_event(
            display, window,
            (XEvent){ .type = X_EVENT_EXPOSE }, X_MASK_ALL);
    return true;
}

bool XGetWindowWorkspace(
    Display *display,
    Window *window,
    uint32_t *workspace)
{
    if (!window_belongs_to(display, window) || !workspace)
        return false;
    *workspace = window->workspace;
    return true;
}

bool XSetCursor(Display *display, XCursorShape shape)
{
    if (!display || shape > X_CURSOR_HIDDEN)
        return false;
    free(display->cursor_pixels);
    display->cursor_pixels = NULL;
    display->cursor_width = 0;
    display->cursor_height = 0;
    display->cursor_shape = shape;
    display->cursor_dirty = true;
    return true;
}

bool XSetCursorImage(
    Display *display,
    uint32_t width,
    uint32_t height,
    const uint32_t *argb_pixels,
    uint32_t stride,
    uint32_t hotspot_x,
    uint32_t hotspot_y)
{
    if (!display || !argb_pixels || !width || !height ||
        width > 64 || height > 64 || stride < width ||
        hotspot_x >= width || hotspot_y >= height ||
        !image_stride_valid(height, stride) ||
        (uint64_t)width * height * sizeof(uint32_t) >
            XLIB_CURSOR_BYTES_MAX)
        return false;
    if (display->cursor_pixels &&
        display->cursor_width == width &&
        display->cursor_height == height &&
        display->cursor_hotspot_x == hotspot_x &&
        display->cursor_hotspot_y == hotspot_y)
    {
        bool unchanged = true;
        for (uint32_t row = 0; row < height; row++)
            if (memcmp(
                    display->cursor_pixels + (size_t)row * width,
                    argb_pixels + (size_t)row * stride,
                    (size_t)width * sizeof(uint32_t)) != 0)
            {
                unchanged = false;
                break;
            }
        if (unchanged)
            return true;
    }
    uint32_t *copy = malloc((size_t)width * height * sizeof(uint32_t));
    if (!copy)
    {
        display->last_error = ENOMEM;
        return false;
    }
    for (uint32_t row = 0; row < height; row++)
        memcpy(
            copy + (size_t)row * width,
            argb_pixels + (size_t)row * stride,
            (size_t)width * sizeof(uint32_t));
    free(display->cursor_pixels);
    display->cursor_pixels = copy;
    display->cursor_width = width;
    display->cursor_height = height;
    display->cursor_hotspot_x = hotspot_x;
    display->cursor_hotspot_y = hotspot_y;
    display->cursor_shape = X_CURSOR_ARROW;
    display->cursor_dirty = true;
    return true;
}

bool XGetPointerPosition(Display *display, int32_t *x, int32_t *y)
{
    if (!display || !x || !y)
        return false;
    *x = display->pointer_x;
    *y = display->pointer_y;
    return true;
}

static uint32_t event_mask_for_type(XEventType type)
{
    switch (type)
    {
        case X_EVENT_KEY_PRESS: return X_MASK_KEY_PRESS;
        case X_EVENT_KEY_RELEASE: return X_MASK_KEY_RELEASE;
        case X_EVENT_BUTTON_PRESS: return X_MASK_BUTTON_PRESS;
        case X_EVENT_BUTTON_RELEASE: return X_MASK_BUTTON_RELEASE;
        case X_EVENT_POINTER_MOTION: return X_MASK_POINTER_MOTION;
        case X_EVENT_FOCUS_IN:
        case X_EVENT_FOCUS_OUT: return X_MASK_FOCUS;
        case X_EVENT_CLOSE_REQUEST: return X_MASK_CLOSE;
        case X_EVENT_CONFIGURE: return X_MASK_CONFIGURE;
        case X_EVENT_MINIMIZE:
        case X_EVENT_RESTORE: return X_MASK_WINDOW_STATE;
        case X_EVENT_WORKSPACE: return X_MASK_WORKSPACE;
        case X_EVENT_EXPOSE:
        case X_EVENT_NONE: return X_MASK_ALL;
    }
    return 0;
}

static void translate_keyboard(Display *display)
{
    os_keyboard_event_t input[16];
    int64_t bytes = os_keyboard_read(input, sizeof(input));
    if (bytes == -EAGAIN)
        return;
    if (bytes < 0)
    {
        display->last_error = (int)-bytes;
        return;
    }
    uint64_t count = (uint64_t)bytes / sizeof(input[0]);
    for (uint64_t i = 0; i < count; i++)
    {
        Window *target = display->focused;
        if (!target)
            target = &display->root;
        XEvent event = {
            .type = input[i].pressed ? X_EVENT_KEY_PRESS : X_EVENT_KEY_RELEASE,
            .keycode = input[i].scancode,
            .character = input[i].character,
            .pressed = input[i].pressed,
            .shift = input[i].shift,
            .caps_lock = input[i].caps_lock,
            .extended = input[i].extended
        };
        (void)queue_event(
            display, target, event, event_mask_for_type(event.type));
    }
}

enum
{
    RESIZE_LEFT = 1U << 0,
    RESIZE_RIGHT = 1U << 1,
    RESIZE_TOP = 1U << 2,
    RESIZE_BOTTOM = 1U << 3
};

static void queue_pointer_event(
    Display *display,
    Window *target,
    XEventType type,
    uint32_t button,
    uint32_t buttons)
{
    int32_t x = display->pointer_x;
    int32_t y = display->pointer_y;
    if (target != &display->root)
    {
        x = clamp_int32((int64_t)x - target->x);
        y = clamp_int32(
            (int64_t)y - target->y -
                window_frame_height(display, target));
    }
    (void)queue_event(
        display, target,
        (XEvent){
            .type = type,
            .x = x,
            .y = y,
            .button = button,
            .buttons = buttons
        },
        event_mask_for_type(type));
}

static uint32_t resize_edges_at(
    const Display *display,
    const Window *window,
    int32_t x,
    int32_t y)
{
    if (!(display->wm_features & X_WM_RESIZE_WINDOWS) ||
        !(window->flags & X_WINDOW_RESIZABLE) ||
        (window->flags & X_WINDOW_FIXED))
        return 0;
    uint32_t edges = 0;
    if ((int64_t)x - window->x < XLIB_RESIZE_BORDER)
        edges |= RESIZE_LEFT;
    else if ((int64_t)window->x + window->width - x <=
             XLIB_RESIZE_BORDER)
        edges |= RESIZE_RIGHT;
    if ((int64_t)y - window->y <
        (int64_t)window_frame_height(display, window) +
            XLIB_RESIZE_BORDER)
        edges |= RESIZE_TOP;
    else if ((int64_t)window->y + window_frame_height(display, window) +
                 window->height - y <= XLIB_RESIZE_BORDER)
        edges |= RESIZE_BOTTOM;
    return edges;
}

static void resize_dragged_window(Display *display)
{
    Window *window = display->dragged;
    int64_t dx = (int64_t)display->pointer_x - display->drag_start_x;
    int64_t dy = (int64_t)display->pointer_y - display->drag_start_y;
    int64_t x = display->drag_window_x;
    int64_t y = display->drag_window_y;
    int64_t width = display->drag_window_width;
    int64_t height = display->drag_window_height;
    if (display->resize_edges & RESIZE_LEFT)
    {
        x += dx;
        width -= dx;
    }
    if (display->resize_edges & RESIZE_RIGHT)
        width += dx;
    if (display->resize_edges & RESIZE_TOP)
    {
        y += dy;
        height -= dy;
    }
    if (display->resize_edges & RESIZE_BOTTOM)
        height += dy;
    if (width < window->minimum_width)
    {
        if (display->resize_edges & RESIZE_LEFT)
            x -= window->minimum_width - width;
        width = window->minimum_width;
    }
    if (height < window->minimum_height)
    {
        if (display->resize_edges & RESIZE_TOP)
            y -= window->minimum_height - height;
        height = window->minimum_height;
    }
    if (width > UINT32_MAX || height > UINT32_MAX ||
        x < INT32_MIN || x > INT32_MAX ||
        y < INT32_MIN || y > INT32_MAX)
        return;
    (void)XMoveResizeWindow(
        display, window, (int32_t)x, (int32_t)y,
        (uint32_t)width, (uint32_t)height);
}

static void begin_window_drag(Display *display, Window *window)
{
    display->dragged = window;
    display->drag_offset_x =
        (int64_t)display->pointer_x - window->x;
    display->drag_offset_y =
        (int64_t)display->pointer_y - window->y;
    display->drag_start_x = display->pointer_x;
    display->drag_start_y = display->pointer_y;
    display->drag_window_x = window->x;
    display->drag_window_y = window->y;
    display->drag_window_width = window->width;
    display->drag_window_height = window->height;
}

static void translate_mouse(Display *display)
{
    os_mouse_event_t input[16];
    int64_t bytes = os_mouse_read(input, sizeof(input));
    if (bytes < 0)
    {
        if (bytes != -EAGAIN && bytes != -ENODEV)
            display->last_error = (int)-bytes;
        return;
    }
    uint64_t count = (uint64_t)bytes / sizeof(input[0]);
    for (uint64_t i = 0; i < count; i++)
    {
        display->pointer_x += input[i].dx;
        display->pointer_y += input[i].dy;
        if (display->pointer_x < 0)
            display->pointer_x = 0;
        if (display->pointer_y < 0)
            display->pointer_y = 0;
        if ((uint64_t)display->pointer_x >= display->screen.width)
            display->pointer_x = (int32_t)display->screen.width - 1;
        if ((uint64_t)display->pointer_y >= display->screen.height)
            display->pointer_y = (int32_t)display->screen.height - 1;
        if (input[i].dx || input[i].dy)
            display->cursor_dirty = true;

        Window *under_pointer =
            window_at(display, display->pointer_x, display->pointer_y);
        if (input[i].dx || input[i].dy)
        {
            Window *target = display->dragged ? display->dragged :
                under_pointer ? under_pointer : &display->root;
            if (display->dragged && display->resize_edges)
                resize_dragged_window(display);
            else if (display->dragged &&
                     (display->wm_features & X_WM_DRAG_WINDOWS))
            {
                int32_t x = clamp_int32(
                    (int64_t)display->pointer_x -
                        display->drag_offset_x);
                int32_t y = clamp_int32(
                    (int64_t)display->pointer_y -
                        display->drag_offset_y);
                if (display->dragged->x != x ||
                    display->dragged->y != y)
                {
                    display->dragged->x = x;
                    display->dragged->y = y;
                    display->scene_dirty = true;
                }
            }
            queue_pointer_event(
                display, target, X_EVENT_POINTER_MOTION, 0,
                input[i].buttons);
        }

        uint32_t changed = display->previous_buttons ^ input[i].buttons;
        for (uint32_t button = 1; button <= 8; button++)
        {
            uint32_t bit = 1U << (button - 1);
            if (!(changed & bit))
                continue;
            Window *target = under_pointer
                ? under_pointer : &display->root;
            bool pressed = (input[i].buttons & bit) != 0;
            if (pressed)
            {
                if (target != &display->root &&
                    (display->wm_features & X_WM_AUTO_RAISE))
                    raise_window(display, target);
                if ((display->wm_features & X_WM_AUTO_FOCUS) &&
                    (target == &display->root ||
                     !(target->flags & X_WINDOW_NO_FOCUS)))
                    set_focus(display, target);
                if (target != &display->root)
                {
                    uint32_t frame =
                        window_frame_height(display, target);
                    bool in_title = frame &&
                        (int64_t)display->pointer_y <
                            (int64_t)target->y + frame;
                    if (in_title)
                    {
                        int64_t title_button_x =
                            (int64_t)display->pointer_x - target->x;
                        if (title_button_x >=
                            (int64_t)target->width - 20)
                        {
                            (void)queue_event(
                                display, target,
                                (XEvent){ .type = X_EVENT_CLOSE_REQUEST },
                                X_MASK_CLOSE);
                        }
                        else if (title_button_x >=
                                 (int64_t)target->width - 40)
                            (void)XMaximizeWindow(display, target);
                        else if (title_button_x >=
                                 (int64_t)target->width - 60)
                            (void)XIconifyWindow(display, target);
                        else if (display->wm_features & X_WM_DRAG_WINDOWS)
                            begin_window_drag(display, target);
                    }
                    else if (display->wm_features & X_WM_RESIZE_WINDOWS)
                    {
                        display->resize_edges = resize_edges_at(
                            display, target,
                            display->pointer_x, display->pointer_y);
                        if (display->resize_edges)
                            begin_window_drag(display, target);
                    }
                }
                queue_pointer_event(
                    display, target, X_EVENT_BUTTON_PRESS, button,
                    input[i].buttons);
            }
            else
            {
                Window *release_target = display->dragged
                    ? display->dragged : target;
                queue_pointer_event(
                    display, release_target, X_EVENT_BUTTON_RELEASE,
                    button, input[i].buttons);
                if (display->dragged)
                {
                    display->dragged = NULL;
                    display->resize_edges = 0;
                }
            }
        }
        display->previous_buttons = input[i].buttons;
    }
}

static void poll_input(Display *display)
{
    translate_mouse(display);
    translate_keyboard(display);
}

bool XPollEvent(Display *display, XEvent *event)
{
    if (!display || !event)
        return false;
    if (!display->event_count)
        poll_input(display);
    if (!display->event_count)
        return false;
    *event = display->events[display->event_read];
    display->event_read =
        (display->event_read + 1) % XLIB_EVENT_QUEUE_SIZE;
    display->event_count--;
    return true;
}

bool XNextEvent(Display *display, XEvent *event)
{
    if (!display || !event)
        return false;
    while (!XPollEvent(display, event))
    {
        if (display->last_error)
            return false;
        yield();
    }
    return true;
}

uint32_t XPending(Display *display)
{
    if (!display)
        return 0;
    if (!display->event_count)
        poll_input(display);
    return display->event_count;
}

static void canvas_fill(Display *display, uint32_t color)
{
    size_t count =
        (size_t)display->screen.width * display->screen.height;
    for (size_t i = 0; i < count; i++)
        display->canvas[i] = color;
}

static void canvas_wallpaper(Display *display)
{
    canvas_fill(display, display->wallpaper_color);
    if (!display->wallpaper_pixels)
        return;
    uint32_t copy_width =
        display->wallpaper_width < display->screen.width
            ? display->wallpaper_width : (uint32_t)display->screen.width;
    uint32_t copy_height =
        display->wallpaper_height < display->screen.height
            ? display->wallpaper_height : (uint32_t)display->screen.height;
    uint32_t offset_x =
        ((uint32_t)display->screen.width - copy_width) / 2;
    uint32_t offset_y =
        ((uint32_t)display->screen.height - copy_height) / 2;
    for (uint32_t row = 0; row < copy_height; row++)
        memcpy(
            display->canvas +
                (size_t)(offset_y + row) * display->screen.width + offset_x,
            display->wallpaper_pixels +
                (size_t)row * display->wallpaper_stride,
            (size_t)copy_width * sizeof(uint32_t));
}

static void canvas_wallpaper_region(
    Display *display,
    const xlib_damage_t *damage)
{
    for (uint32_t row = 0; row < damage->height; row++)
    {
        uint32_t *pixels = display->canvas +
            (size_t)(damage->y + row) * display->screen.width + damage->x;
        for (uint32_t column = 0; column < damage->width; column++)
            pixels[column] = display->wallpaper_color;
    }

    if (!display->wallpaper_pixels)
        return;
    uint32_t copy_width =
        display->wallpaper_width < display->screen.width
            ? display->wallpaper_width : (uint32_t)display->screen.width;
    uint32_t copy_height =
        display->wallpaper_height < display->screen.height
            ? display->wallpaper_height : (uint32_t)display->screen.height;
    uint32_t offset_x =
        ((uint32_t)display->screen.width - copy_width) / 2;
    uint32_t offset_y =
        ((uint32_t)display->screen.height - copy_height) / 2;
    uint32_t left = damage->x > offset_x ? damage->x : offset_x;
    uint32_t top = damage->y > offset_y ? damage->y : offset_y;
    uint32_t right = damage->x + damage->width;
    uint32_t bottom = damage->y + damage->height;
    uint32_t wallpaper_right = offset_x + copy_width;
    uint32_t wallpaper_bottom = offset_y + copy_height;
    if (right > wallpaper_right)
        right = wallpaper_right;
    if (bottom > wallpaper_bottom)
        bottom = wallpaper_bottom;
    if (right <= left || bottom <= top)
        return;
    for (uint32_t row = top; row < bottom; row++)
    {
        memcpy(
            display->canvas +
                (size_t)row * display->screen.width + left,
            display->wallpaper_pixels +
                (size_t)(row - offset_y) * display->wallpaper_stride +
                (left - offset_x),
            (size_t)(right - left) * sizeof(uint32_t));
    }
}

static void canvas_pixel(
    Display *display,
    int64_t x,
    int64_t y,
    uint32_t color)
{
    if (x < 0 || y < 0 ||
        (uint64_t)x >= display->screen.width ||
        (uint64_t)y >= display->screen.height)
        return;
    display->canvas[(size_t)y * display->screen.width + (size_t)x] = color;
}

static void canvas_window(Display *display, Window *window)
{
    uint32_t frame = window_frame_height(display, window);
    int64_t origin_x = window->x;
    int64_t origin_y = window->y + frame;
    for (uint32_t row = 0; row < window->height; row++)
    {
        int64_t target_y = origin_y + row;
        if (target_y < 0 || (uint64_t)target_y >= display->screen.height)
            continue;
        for (uint32_t column = 0; column < window->width; column++)
        {
            int64_t target_x = origin_x + column;
            if (target_x < 0 ||
                (uint64_t)target_x >= display->screen.width)
                continue;
            canvas_pixel(
                display, target_x, target_y,
                window->pixels[(size_t)row * window->width + column]);
        }
    }
    if (frame)
    {
        uint32_t title_color = window->focused
            ? display->decoration_theme.active_title
            : display->decoration_theme.inactive_title;
        for (uint32_t row = 0; row < XLIB_TITLE_HEIGHT; row++)
            for (uint32_t column = 0; column < window->width; column++)
                canvas_pixel(
                    display, origin_x + column, window->y + row,
                    title_color);
        for (uint32_t i = 0; i < XLIB_BORDER_SIZE; i++)
        {
            for (uint32_t column = 0; column < window->width; column++)
            {
                canvas_pixel(
                    display, origin_x + column, window->y + i,
                    display->decoration_theme.border);
                canvas_pixel(
                    display, origin_x + column,
                    origin_y + window->height - 1 - i,
                    display->decoration_theme.border);
            }
            for (uint32_t row = 0;
                 row < window->height + XLIB_TITLE_HEIGHT; row++)
            {
                canvas_pixel(
                    display, origin_x + i, window->y + row,
                    display->decoration_theme.border);
                canvas_pixel(
                    display, origin_x + window->width - 1 - i,
                    window->y + row, display->decoration_theme.border);
            }
        }
        uint32_t button_count = 3;
        for (uint32_t button = 0; button < button_count; button++)
        {
            int64_t button_left =
                origin_x + window->width -
                (int64_t)(button + 1) * 20;
            uint32_t button_color = button == 0
                ? display->decoration_theme.close_background
                : display->decoration_theme.button_background;
            for (uint32_t row = 0; row < 16; row++)
                for (uint32_t column = 0; column < 18; column++)
                    canvas_pixel(
                        display, button_left + column + 1,
                        window->y + 4 + row, button_color);

            uint32_t glyph = display->decoration_theme.button_foreground;
            int64_t center_x = button_left + 10;
            int64_t center_y = window->y + 12;
            if (button == 0)
            {
                for (int32_t offset = -3; offset <= 3; offset++)
                {
                    canvas_pixel(
                        display, center_x + offset,
                        center_y + offset, glyph);
                    canvas_pixel(
                        display, center_x + offset,
                        center_y - offset, glyph);
                }
            }
            else if (button == 1)
            {
                for (int32_t offset = -4; offset <= 4; offset++)
                {
                    canvas_pixel(
                        display, center_x + offset, center_y - 4, glyph);
                    canvas_pixel(
                        display, center_x + offset, center_y + 4, glyph);
                    canvas_pixel(
                        display, center_x - 4, center_y + offset, glyph);
                    canvas_pixel(
                        display, center_x + 4, center_y + offset, glyph);
                }
            }
            else if (button == 2)
            {
                for (int32_t offset = -4; offset <= 4; offset++)
                    canvas_pixel(
                        display, center_x + offset, center_y + 4, glyph);
            }
            else
            {
                for (int32_t offset = -4; offset <= 4; offset++)
                {
                    canvas_pixel(
                        display, center_x + offset,
                        center_y + (offset < 0 ? -offset / 2 : offset / 2),
                        glyph);
                    canvas_pixel(
                        display, center_x + offset,
                        center_y - (offset < 0 ? -offset / 2 : offset / 2),
                        glyph);
                }
                canvas_pixel(display, center_x, center_y, glyph);
                for (int32_t offset = -4; offset <= 4; offset++)
                    canvas_pixel(
                        display, center_x + offset,
                        center_y - offset, glyph);
            }
        }
    }
}

static void canvas_pixel_in_region(
    Display *display,
    const xlib_damage_t *damage,
    int64_t x,
    int64_t y,
    uint32_t color)
{
    if (x < damage->x || y < damage->y ||
        x >= (int64_t)damage->x + damage->width ||
        y >= (int64_t)damage->y + damage->height)
        return;
    canvas_pixel(display, x, y, color);
}

static void canvas_window_region(
    Display *display,
    Window *window,
    const xlib_damage_t *damage)
{
    uint32_t frame = window_frame_height(display, window);
    int64_t origin_x = window->x;
    int64_t origin_y = (int64_t)window->y + frame;
    int64_t left = damage->x > origin_x ? damage->x : origin_x;
    int64_t right = (int64_t)damage->x + damage->width;
    if (right > origin_x + window->width)
        right = origin_x + window->width;
    int64_t top = damage->y > origin_y ? damage->y : origin_y;
    int64_t bottom = (int64_t)damage->y + damage->height;
    if (bottom > origin_y + window->height)
        bottom = origin_y + window->height;
    for (int64_t y = top; y < bottom; y++)
        for (int64_t x = left; x < right; x++)
            canvas_pixel(
                display, x, y,
                window->pixels[(size_t)(y - origin_y) * window->width +
                               (size_t)(x - origin_x)]);

    if (!frame)
        return;

    int64_t damage_left = damage->x;
    int64_t damage_top = damage->y;
    int64_t damage_right = damage_left + damage->width;
    int64_t damage_bottom = damage_top + damage->height;
    int64_t frame_left =
        damage_left > origin_x ? damage_left : origin_x;
    int64_t frame_right = damage_right;
    if (frame_right > origin_x + window->width)
        frame_right = origin_x + window->width;
    int64_t frame_top =
        damage_top > window->y ? damage_top : window->y;
    int64_t frame_bottom = damage_bottom;
    if (frame_bottom > origin_y)
        frame_bottom = origin_y;
    uint32_t title_color = window->focused
        ? display->decoration_theme.active_title
        : display->decoration_theme.inactive_title;
    for (int64_t y = frame_top; y < frame_bottom; y++)
        for (int64_t x = frame_left; x < frame_right; x++)
            canvas_pixel(display, x, y, title_color);

    for (uint32_t i = 0; i < XLIB_BORDER_SIZE; i++)
    {
        int64_t top_border_y = (int64_t)window->y + i;
        int64_t bottom_border_y =
            origin_y + window->height - 1 - i;
        for (int64_t x = frame_left; x < frame_right; x++)
        {
            canvas_pixel(
                display, x, top_border_y,
                display->decoration_theme.border);
            canvas_pixel(
                display, x, bottom_border_y,
                display->decoration_theme.border);
        }

        int64_t side_top =
            damage_top > window->y ? damage_top : window->y;
        int64_t side_bottom = damage_bottom;
        if (side_bottom > origin_y + window->height)
            side_bottom = origin_y + window->height;
        for (int64_t y = side_top; y < side_bottom; y++)
        {
            canvas_pixel(
                display, origin_x + i, y,
                display->decoration_theme.border);
            canvas_pixel(
                display, origin_x + window->width - 1 - i, y,
                display->decoration_theme.border);
        }
    }

    for (uint32_t button = 0; button < 3; button++)
    {
        int64_t button_left =
            origin_x + window->width - (int64_t)(button + 1) * 20;
        int64_t button_right = button_left + 19;
        int64_t button_top = (int64_t)window->y + 4;
        int64_t button_bottom = button_top + 16;
        int64_t x0 = damage->x > button_left ? damage->x : button_left;
        int64_t x1 = (int64_t)damage->x + damage->width;
        if (x1 > button_right)
            x1 = button_right;
        int64_t y0 = damage->y > button_top ? damage->y : button_top;
        int64_t y1 = (int64_t)damage->y + damage->height;
        if (y1 > button_bottom)
            y1 = button_bottom;
        uint32_t button_color = button == 0
            ? display->decoration_theme.close_background
            : display->decoration_theme.button_background;
        for (int64_t y = y0; y < y1; y++)
            for (int64_t x = x0; x < x1; x++)
                canvas_pixel(display, x, y, button_color);

        uint32_t glyph = display->decoration_theme.button_foreground;
        int64_t center_x = button_left + 10;
        int64_t center_y = window->y + 12;
        if (button == 0)
        {
            for (int32_t offset = -3; offset <= 3; offset++)
            {
                canvas_pixel_in_region(
                    display, damage, center_x + offset,
                    center_y + offset, glyph);
                canvas_pixel_in_region(
                    display, damage, center_x + offset,
                    center_y - offset, glyph);
            }
        }
        else if (button == 1)
        {
            for (int32_t offset = -4; offset <= 4; offset++)
            {
                canvas_pixel_in_region(
                    display, damage, center_x + offset,
                    center_y - 4, glyph);
                canvas_pixel_in_region(
                    display, damage, center_x + offset,
                    center_y + 4, glyph);
                canvas_pixel_in_region(
                    display, damage, center_x - 4,
                    center_y + offset, glyph);
                canvas_pixel_in_region(
                    display, damage, center_x + 4,
                    center_y + offset, glyph);
            }
        }
        else
        {
            for (int32_t offset = -4; offset <= 4; offset++)
                canvas_pixel_in_region(
                    display, damage, center_x + offset,
                    center_y + 4, glyph);
        }
    }
}

static bool present_canvas(Display *display)
{
    os_screen_region_t region = {
        .x = 0,
        .y = 0,
        .width = display->screen.width,
        .height = display->screen.height,
        .stride = display->screen.width,
        .pixels = display->canvas
    };
    if (os_screen_blit(&region) < 0)
    {
        display->last_error = EIO;
        return false;
    }
    return true;
}

static bool present_canvas_region(
    Display *display,
    const xlib_damage_t *damage)
{
    os_screen_region_t region = {
        .x = damage->x,
        .y = damage->y,
        .width = damage->width,
        .height = damage->height,
        .stride = display->screen.width,
        .pixels = display->canvas +
            (size_t)damage->y * display->screen.width + damage->x
    };
    if (os_screen_blit(&region) < 0)
    {
        display->last_error = EIO;
        return false;
    }
    return true;
}

static bool text_is_visible(
    Display *display,
    Window *window,
    int64_t x,
    int64_t y,
    size_t length,
    bool title)
{
    int64_t width = (int64_t)length * 8;
    int64_t height = 16;
    uint32_t frame = window_frame_height(display, window);
    int64_t window_top = (int64_t)window->y +
        (title ? 0 : frame);
    int64_t window_bottom = window_top +
        (title ? frame : window->height);
    if (x < window->x ||
        x + width > (int64_t)window->x + window->width ||
        y < window_top || y + height > window_bottom ||
        x < 0 || y < 0 ||
        x + width > (int64_t)display->screen.width ||
        y + height > (int64_t)display->screen.height)
        return false;

    uint32_t index = 0;
    while (index < display->window_count &&
           display->windows[index] != window)
        index++;
    for (uint32_t i = index + 1; i < display->window_count; i++)
    {
        Window *above = display->windows[i];
        if (!above->mapped)
            continue;
        int64_t above_left = above->x;
        int64_t above_top = above->y;
        int64_t above_right = above_left + above->width;
        int64_t above_bottom = above_top +
                window_frame_height(display, above) + above->height;
            if (!(above->flags & X_WINDOW_STICKY) &&
                above->workspace != display->current_workspace)
                continue;
        if (x < above_right && x + width > above_left &&
            y < above_bottom && y + height > above_top)
            return false;
    }
    return true;
}

static bool text_intersects_damage(
    const xlib_damage_t *damage,
    int64_t x,
    int64_t y,
    size_t length)
{
    if (!damage)
        return true;
    int64_t right = x + (int64_t)length * 8;
    int64_t bottom = y + 16;
    return x < (int64_t)damage->x + damage->width &&
        right > damage->x &&
        y < (int64_t)damage->y + damage->height &&
        bottom > damage->y;
}

static bool draw_window_text(
    Display *display,
    Window *window,
    const xlib_damage_t *damage)
{
    os_draw_command_t commands[XLIB_DRAW_BATCH_SIZE];
    uint32_t count = 0;
    if (window_frame_height(display, window) && window->title[0])
    {
        int64_t title_x = (int64_t)window->x + 8;
        int64_t title_y = (int64_t)window->y + 5;
        size_t length = strlen(window->title);
        uint32_t button_count = 3;
        uint32_t title_width = window->width > 8 + button_count * 20 + 6
            ? window->width - 8 - button_count * 20 - 6
            : 0;
        if (length > title_width / 8)
            length = title_width / 8;
        if (text_intersects_damage(
                damage, title_x, title_y, length) &&
            text_is_visible(
                display, window, title_x, title_y, length, true))
        {
            os_draw_command_t *command = &commands[count++];
            *command = (os_draw_command_t){
                .type = OS_DRAW_TEXT,
                .x = (uint32_t)title_x,
                .y = (uint32_t)title_y,
                .color = display->decoration_theme.title_text
            };
            if (length > sizeof(command->text))
                length = sizeof(command->text);
            memcpy(command->text, window->title, length);
            command->text_length = (uint32_t)length;
        }
    }
    for (uint32_t i = 0; i < window->text_count; i++)
    {
        if (count == XLIB_DRAW_BATCH_SIZE)
        {
            if (os_draw_batch(commands, count) < 0)
            {
                display->last_error = EIO;
                return false;
            }
            count = 0;
        }
        int64_t text_x = (int64_t)window->x + window->text[i].x;
        int64_t text_y = (int64_t)window->y +
            window_frame_height(display, window) + window->text[i].y;
        size_t length = strlen(window->text[i].text);
        if (!text_intersects_damage(
                damage, text_x, text_y, length) ||
            !text_is_visible(
                display, window, text_x, text_y, length, false))
            continue;
        os_draw_command_t *command = &commands[count++];
        *command = (os_draw_command_t){
            .type = OS_DRAW_TEXT,
            .x = (uint32_t)text_x,
            .y = (uint32_t)text_y,
            .color = window->text[i].color
        };
        if (length > sizeof(command->text))
            length = sizeof(command->text);
        memcpy(command->text, window->text[i].text, length);
        command->text_length = (uint32_t)length;
    }
    if (count && os_draw_batch(commands, count) < 0)
    {
        display->last_error = EIO;
        return false;
    }
    return true;
}

static bool cursor_rectangle(
    const Display *display,
    uint32_t *x,
    uint32_t *y,
    uint32_t *width,
    uint32_t *height)
{
    int64_t left = display->pointer_x;
    int64_t top = display->pointer_y;
    uint32_t cursor_width = 0;
    uint32_t cursor_height = 0;
    if (display->cursor_pixels)
    {
        left -= display->cursor_hotspot_x;
        top -= display->cursor_hotspot_y;
        cursor_width = display->cursor_width;
        cursor_height = display->cursor_height;
    }
    else
    {
        switch (display->cursor_shape)
        {
            case X_CURSOR_ARROW:
                cursor_width = 12;
                cursor_height = 15;
                break;
            case X_CURSOR_IBEAM:
                left -= 4;
                cursor_width = 9;
                cursor_height = 16;
                break;
            case X_CURSOR_HAND:
                left += 1;
                cursor_width = 11;
                cursor_height = 12;
                break;
            case X_CURSOR_CROSSHAIR:
                left -= 7;
                top -= 7;
                cursor_width = 15;
                cursor_height = 15;
                break;
            case X_CURSOR_HIDDEN:
                return false;
        }
    }

    int64_t right = left + cursor_width;
    int64_t bottom = top + cursor_height;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > (int64_t)display->screen.width)
        right = (int64_t)display->screen.width;
    if (bottom > (int64_t)display->screen.height)
        bottom = (int64_t)display->screen.height;
    if (right <= left || bottom <= top)
        return false;

    *x = (uint32_t)left;
    *y = (uint32_t)top;
    *width = (uint32_t)(right - left);
    *height = (uint32_t)(bottom - top);
    return *width <= XLIB_CURSOR_PIXEL_MAX &&
        *height <= XLIB_CURSOR_PIXEL_MAX;
}

static bool draw_cursor(Display *display);

static bool restore_cursor_underlay(Display *display)
{
    if (!display->cursor_underlay_valid)
        return true;
    os_screen_region_t previous = {
        .x = display->cursor_underlay_x,
        .y = display->cursor_underlay_y,
        .width = display->cursor_underlay_width,
        .height = display->cursor_underlay_height,
        .stride = display->cursor_underlay_width,
        .pixels = display->cursor_underlay
    };
    if (os_screen_blit(&previous) < 0)
    {
        display->last_error = EIO;
        return false;
    }
    display->cursor_underlay_valid = false;
    return true;
}

static bool update_cursor_overlay(
    Display *display,
    bool restore_previous)
{
    if (restore_previous && !restore_cursor_underlay(display))
        return false;
    display->cursor_underlay_valid = false;

    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    if (!cursor_rectangle(display, &x, &y, &width, &height))
        return display->cursor_shape == X_CURSOR_HIDDEN;

    os_screen_region_t current = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .stride = width,
        .pixels = display->cursor_underlay
    };
    if (os_screen_read(&current) < 0 || !draw_cursor(display))
    {
        display->last_error = EIO;
        return false;
    }

    display->cursor_underlay_x = x;
    display->cursor_underlay_y = y;
    display->cursor_underlay_width = width;
    display->cursor_underlay_height = height;
    display->cursor_underlay_valid = true;
    return true;
}

static bool draw_cursor(Display *display)
{
    if (display->cursor_pixels)
    {
        os_draw_command_t commands[XLIB_DRAW_BATCH_SIZE];
        uint32_t count = 0;
        int64_t origin_x =
            (int64_t)display->pointer_x - display->cursor_hotspot_x;
        int64_t origin_y =
            (int64_t)display->pointer_y - display->cursor_hotspot_y;
        for (uint32_t y = 0; y < display->cursor_height; y++)
        {
            uint32_t x = 0;
            while (x < display->cursor_width)
            {
                uint32_t pixel = display->cursor_pixels[
                    (size_t)y * display->cursor_width + x];
                if (!(pixel & 0xFF000000U))
                {
                    x++;
                    continue;
                }
                uint32_t run = 1;
                while (x + run < display->cursor_width &&
                       display->cursor_pixels[
                           (size_t)y * display->cursor_width + x + run] ==
                           pixel)
                    run++;
                int64_t screen_x = origin_x + x;
                int64_t screen_y = origin_y + y;
                int64_t run_end = screen_x + run;
                if (screen_y >= 0 &&
                    (uint64_t)screen_y < display->screen.height &&
                    run_end > 0 &&
                    screen_x < (int64_t)display->screen.width)
                {
                    if (count == XLIB_DRAW_BATCH_SIZE)
                    {
                        if (os_draw_batch(commands, count) < 0)
                        {
                            display->last_error = EIO;
                            return false;
                        }
                        count = 0;
                    }
                    int64_t visible_x = screen_x < 0 ? 0 : screen_x;
                    int64_t visible_end =
                        run_end > (int64_t)display->screen.width
                            ? (int64_t)display->screen.width : run_end;
                    uint32_t visible_width =
                        (uint32_t)(visible_end - visible_x);
                    commands[count++] = (os_draw_command_t){
                        .type = OS_DRAW_RECTANGLE,
                        .x = (uint32_t)visible_x,
                        .y = (uint32_t)screen_y,
                        .width = visible_width,
                        .height = 1,
                        .color = pixel & 0x00FFFFFFU
                    };
                }
                x += run;
            }
        }
        if (count && os_draw_batch(commands, count) < 0)
        {
            display->last_error = EIO;
            return false;
        }
        return true;
    }
    if (display->cursor_shape == X_CURSOR_HIDDEN)
        return true;
    os_draw_command_t commands[6];
    uint32_t count = 0;
#define CURSOR_RECT(cx, cy, cw, ch, cc) \
    commands[count++] = (os_draw_command_t){ \
        .type = OS_DRAW_RECTANGLE, \
        .x = (uint32_t)(((int64_t)display->pointer_x + (cx)) < 0 \
            ? 0 : (int64_t)display->pointer_x + (cx)), \
        .y = (uint32_t)(((int64_t)display->pointer_y + (cy)) < 0 \
            ? 0 : (int64_t)display->pointer_y + (cy)), \
        .width = (cw), .height = (ch), .color = (cc) }
    switch (display->cursor_shape)
    {
        case X_CURSOR_ARROW:
            CURSOR_RECT(0, 0, 2, 15, XRGB(255, 255, 255));
            CURSOR_RECT(2, 2, 2, 8, XRGB(255, 255, 255));
            CURSOR_RECT(4, 4, 2, 6, XRGB(255, 255, 255));
            CURSOR_RECT(6, 6, 2, 5, XRGB(255, 255, 255));
            CURSOR_RECT(8, 8, 2, 7, XRGB(255, 255, 255));
            CURSOR_RECT(4, 10, 4, 2, XRGB(255, 255, 255));
            break;
        case X_CURSOR_IBEAM:
            CURSOR_RECT(-1, 0, 3, 16, XRGB(255, 255, 255));
            CURSOR_RECT(-4, 0, 9, 2, XRGB(255, 255, 255));
            CURSOR_RECT(-4, 14, 9, 2, XRGB(255, 255, 255));
            break;
        case X_CURSOR_HAND:
            CURSOR_RECT(4, 0, 5, 12, XRGB(255, 255, 255));
            CURSOR_RECT(1, 6, 11, 6, XRGB(255, 255, 255));
            break;
        case X_CURSOR_CROSSHAIR:
            CURSOR_RECT(-7, -1, 15, 3, XRGB(255, 255, 255));
            CURSOR_RECT(-1, -7, 3, 15, XRGB(255, 255, 255));
            break;
        case X_CURSOR_HIDDEN:
            break;
    }
#undef CURSOR_RECT
    for (uint32_t i = 0; i < count; i++)
    {
        if ((int64_t)commands[i].x + commands[i].width >
                display->screen.width ||
            (int64_t)commands[i].y + commands[i].height >
                display->screen.height)
        {
            if (commands[i].x >= display->screen.width)
                commands[i].width = 0;
            else if ((int64_t)commands[i].x + commands[i].width >
                     display->screen.width)
                commands[i].width =
                    (uint32_t)display->screen.width - commands[i].x;
            if (commands[i].y >= display->screen.height)
                commands[i].height = 0;
            else if ((int64_t)commands[i].y + commands[i].height >
                     display->screen.height)
                commands[i].height =
                    (uint32_t)display->screen.height - commands[i].y;
        }
    }
    if (count && os_draw_batch(commands, count) < 0)
    {
        display->last_error = EIO;
        return false;
    }
    return true;
}

bool XFlush(Display *display)
{
    if (!display)
        return false;
    display->last_error = 0;
    if (!display->scene_dirty && !display->damage_valid &&
        !display->cursor_dirty)
        return true;

    if (!display->scene_dirty)
    {
        if (display->damage_valid)
        {
            canvas_wallpaper_region(display, &display->damage);
            for (uint32_t i = 0; i < display->window_count; i++)
            {
                Window *window = display->windows[i];
                if (!window->mapped ||
                    (!(window->flags & X_WINDOW_STICKY) &&
                     window->workspace != display->current_workspace))
                    continue;
                canvas_window_region(
                    display, window, &display->damage);
            }
            if (!restore_cursor_underlay(display) ||
                !present_canvas_region(display, &display->damage))
                return false;
            for (uint32_t i = 0; i < display->window_count; i++)
            {
                Window *window = display->windows[i];
                if (window->mapped &&
                    ((window->flags & X_WINDOW_STICKY) ||
                     window->workspace == display->current_workspace) &&
                    !draw_window_text(display, window, &display->damage))
                    return false;
            }
            display->damage_valid = false;
            display->cursor_dirty = false;
            return update_cursor_overlay(display, false);
        }
        if (!update_cursor_overlay(display, true))
            return false;
        display->cursor_dirty = false;
        return true;
    }

    canvas_wallpaper(display);
    for (uint32_t i = 0; i < display->window_count; i++)
    {
        Window *window = display->windows[i];
        if (!window->mapped ||
            (!(window->flags & X_WINDOW_STICKY) &&
             window->workspace != display->current_workspace))
            continue;
        canvas_window(display, window);
    }
    display->cursor_underlay_valid = false;
    if (!present_canvas(display))
        return false;
    for (uint32_t i = 0; i < display->window_count; i++)
        if (display->windows[i]->mapped &&
            ((display->windows[i]->flags & X_WINDOW_STICKY) ||
             display->windows[i]->workspace ==
                 display->current_workspace) &&
            !draw_window_text(display, display->windows[i], NULL))
            return false;
    display->scene_dirty = false;
    display->damage_valid = false;
    if (!update_cursor_overlay(display, false))
    {
        display->scene_dirty = true;
        return false;
    }
    display->cursor_dirty = false;
    return true;
}

int XLastError(const Display *display)
{
    return display ? display->last_error : EINVAL;
}

#undef XLIB_DRAW_BATCH_SIZE
