#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <X11/Xdesktop.h>
#include <X11/Xwidgets.h>
#include <os/gui.h>

#define TEST_WIDTH 320
#define TEST_HEIGHT 240
#define TEST_INPUT_LIMIT 16

static uint32_t screen_pixels[TEST_WIDTH * TEST_HEIGHT];
static uint32_t screen_blit_count;
static os_screen_region_t last_screen_blit;
static os_mouse_event_t mouse_events[TEST_INPUT_LIMIT];
static uint32_t mouse_event_count;
static os_keyboard_event_t keyboard_events[TEST_INPUT_LIMIT];
static uint32_t keyboard_event_count;

int64_t os_screen_info(os_screen_info_t *info)
{
    if (!info)
        return -EINVAL;
    info->width = TEST_WIDTH;
    info->height = TEST_HEIGHT;
    return 0;
}

int64_t os_screen_blit(const os_screen_region_t *region)
{
    if (!region || !region->pixels || !region->width || !region->height ||
        region->stride < region->width ||
        region->x > TEST_WIDTH || region->y > TEST_HEIGHT ||
        region->width > TEST_WIDTH - region->x ||
        region->height > TEST_HEIGHT - region->y)
        return -EINVAL;
    for (uint32_t y = 0; y < region->height; y++)
        memcpy(
            screen_pixels + (size_t)(region->y + y) * TEST_WIDTH + region->x,
            region->pixels + (size_t)y * region->stride,
            (size_t)region->width * sizeof(uint32_t));
    last_screen_blit = *region;
    screen_blit_count++;
    return 0;
}

int64_t os_screen_read(os_screen_region_t *region)
{
    if (!region || !region->pixels || !region->width || !region->height ||
        region->stride < region->width ||
        region->x > TEST_WIDTH || region->y > TEST_HEIGHT ||
        region->width > TEST_WIDTH - region->x ||
        region->height > TEST_HEIGHT - region->y)
        return -EINVAL;
    for (uint32_t y = 0; y < region->height; y++)
        memcpy(
            (uint32_t *)region->pixels + (size_t)y * region->stride,
            screen_pixels + (size_t)(region->y + y) * TEST_WIDTH + region->x,
            (size_t)region->width * sizeof(uint32_t));
    return 0;
}

int64_t os_draw_batch(const os_draw_command_t *commands, uint64_t count)
{
    if (count && !commands)
        return -EINVAL;
    for (uint64_t i = 0; i < count; i++)
    {
        if (commands[i].type != OS_DRAW_RECTANGLE)
            continue;
        uint64_t right = (uint64_t)commands[i].x + commands[i].width;
        uint64_t bottom = (uint64_t)commands[i].y + commands[i].height;
        for (uint32_t y = commands[i].y;
             y < bottom && y < TEST_HEIGHT; y++)
            for (uint32_t x = commands[i].x;
                 x < right && x < TEST_WIDTH; x++)
                screen_pixels[(size_t)y * TEST_WIDTH + x] =
                    commands[i].color;
    }
    return 0;
}

int64_t os_mouse_read(os_mouse_event_t *events, uint64_t size)
{
    uint64_t capacity = size / sizeof(*events);
    uint32_t count = mouse_event_count;
    if (count > capacity)
        count = (uint32_t)capacity;
    if (!count)
        return -EAGAIN;
    memcpy(events, mouse_events, count * sizeof(*events));
    memmove(
        mouse_events, mouse_events + count,
        (mouse_event_count - count) * sizeof(*events));
    mouse_event_count -= count;
    return (int64_t)(count * sizeof(*events));
}

int64_t os_keyboard_read(os_keyboard_event_t *events, uint64_t size)
{
    uint64_t capacity = size / sizeof(*events);
    uint32_t count = keyboard_event_count;
    if (count > capacity)
        count = (uint32_t)capacity;
    if (!count)
        return -EAGAIN;
    memcpy(events, keyboard_events, count * sizeof(*events));
    memmove(
        keyboard_events, keyboard_events + count,
        (keyboard_event_count - count) * sizeof(*events));
    keyboard_event_count -= count;
    return (int64_t)(count * sizeof(*events));
}

void yield(void)
{
}

static void push_mouse(int16_t dx, int16_t dy, uint8_t buttons)
{
    assert(mouse_event_count < TEST_INPUT_LIMIT);
    mouse_events[mouse_event_count++] = (os_mouse_event_t){
        .dx = dx,
        .dy = dy,
        .buttons = buttons
    };
}

static void discard_events(Display *display)
{
    XEvent event;
    while (XPollEvent(display, &event))
    {
    }
}

int main(void)
{
    XEvent event;
    XWindowAttributes attributes;
    Display *display = XOpenDisplay();
    assert(display);

    uint32_t screen_width = 0;
    uint32_t screen_height = 0;
    assert(XGetScreenSize(display, &screen_width, &screen_height));
    assert(screen_width == TEST_WIDTH && screen_height == TEST_HEIGHT);

    Window *root = XDefaultRootWindow(display);
    assert(root);
    assert(XSelectInput(
        display, root,
        X_MASK_BUTTON_PRESS | X_MASK_BUTTON_RELEASE | X_MASK_KEY_PRESS));

    Window *application = XCreateSimpleWindow(
        display, root, 10, 10, 200, 150,
        XRGB(180, 20, 20), "Application");
    Window *taskbar = XCreateSimpleWindow(
        display, root, 0, TEST_HEIGHT - 20, TEST_WIDTH, 20,
        XRGB(20, 30, 40), "Taskbar");
    assert(application && taskbar);
    assert(XSetWindowFlags(
        display, taskbar,
        X_WINDOW_UNDECORATED | X_WINDOW_ALWAYS_ON_TOP |
            X_WINDOW_FIXED | X_WINDOW_SKIP_TASKBAR | X_WINDOW_NO_FOCUS |
            X_WINDOW_STICKY));
    assert(XSetWindowMinimumSize(display, application, 100, 80));
    assert(XFillRectangle(
        display, application, XRGB(200, 10, 10),
        0, 0, 200, 150));
    assert(XFillRectangle(
        display, taskbar, XRGB(10, 20, 220),
        0, 0, TEST_WIDTH, 20));
    assert(XDrawString(
        display, taskbar, XRGB(255, 255, 255), 8, 1, "Applications"));
    assert(XSelectInput(display, taskbar, X_MASK_ALL));
    assert(XMapWindow(display, application));
    assert(XMapWindow(display, taskbar));
    discard_events(display);

    assert(XGetWorkspaceCount(display) == 4);
    assert(XSetWorkspaceCount(display, 3));
    assert(XSetWindowWorkspace(display, application, 1));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.workspace == 1);
    assert(strcmp(attributes.title, "Application") == 0);
    assert(XWindowAtPosition(display, 30, 30) == root);
    assert(XSetCurrentWorkspace(display, 1));
    assert(XWindowAtPosition(display, 30, 30) == application);
    assert(XWindowAtPosition(display, 4, TEST_HEIGHT - 4) == taskbar);
    assert(XSetCurrentWorkspace(display, 0));
    assert(XSetWindowWorkspace(display, application, 0));
    discard_events(display);

    Window *listed[XLIB_MAX_WINDOWS];
    uint32_t listed_count = 0;
    assert(XListWindows(
        display, listed, XLIB_MAX_WINDOWS, &listed_count));
    assert(listed_count == 1 && listed[0] == application);
    assert(XWindowAtPosition(display, 4, TEST_HEIGHT - 4) == taskbar);
    assert(XWindowAtPosition(display, 30, 30) == application);

    assert(XFlush(display));
    assert(screen_pixels[230 * TEST_WIDTH + 10] == XRGB(10, 20, 220));
    assert(screen_pixels[100 * TEST_WIDTH + 20] == XRGB(200, 10, 10));

    assert(XFillRectangle(
        display, application, XRGB(10, 210, 30), 4, 7, 12, 9));
    assert(XFlush(display));
    assert(last_screen_blit.x == 14 && last_screen_blit.y == 41);
    assert(last_screen_blit.width == 12 && last_screen_blit.height == 9);
    assert(screen_pixels[41 * TEST_WIDTH + 14] == XRGB(10, 210, 30));

    assert(XDrawString(
        display, application, XRGB(255, 255, 255), 40, 30, "typing"));
    assert(XFlush(display));
    assert(last_screen_blit.x == 50 && last_screen_blit.y == 64);
    assert(last_screen_blit.width == 48 && last_screen_blit.height == 16);

    XWidgetSet *widgets = XWidgetSetCreate(display, application);
    assert(widgets);
    XWidgetTheme theme = {
        .background = XRGB(30, 35, 44),
        .foreground = XRGB(240, 240, 240),
        .accent = XRGB(40, 120, 220),
        .border = XRGB(90, 100, 115),
        .disabled = XRGB(100, 100, 100)
    };
    assert(XWidgetSetSetTheme(widgets, &theme));
    XWidget *button =
        XWidgetAddButton(widgets, 1, 10, 10, 80, 24, "Launch");
    XWidget *checkbox =
        XWidgetAddCheckbox(widgets, 2, 10, 40, 140, 24, "Enabled", false);
    XWidget *slider =
        XWidgetAddSlider(widgets, 3, 10, 70, 100, 20, 0, 100, 50);
    XWidget *progress =
        XWidgetAddProgressBar(widgets, 4, 10, 100, 120, 20, 0, 100, 40);
    XWidget *text_field =
        XWidgetAddTextField(widgets, 5, 10, 125, 170, 20, 16, "hi");
    assert(button && checkbox && slider && progress && text_field);
    assert(!XWidgetAddButton(widgets, 1, 0, 0, 40, 20, "Duplicate"));

    XWidgetEvent widget_event;
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_PRESS,
            .window = application,
            .x = 20,
            .y = 20,
            .button = 1
        },
        &widget_event));
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_RELEASE,
            .window = application,
            .x = 20,
            .y = 20,
            .button = 1
        },
        &widget_event));
    assert(widget_event.type == X_WIDGET_EVENT_ACTIVATED);
    assert(widget_event.id == 1);

    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_PRESS,
            .window = application,
            .x = 20,
            .y = 50,
            .button = 1
        },
        &widget_event));
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_RELEASE,
            .window = application,
            .x = 20,
            .y = 50,
            .button = 1
        },
        &widget_event));
    uint32_t widget_value = 0;
    assert(widget_event.type == X_WIDGET_EVENT_CHANGED);
    assert(XWidgetGetValue(checkbox, &widget_value) && widget_value == 1);

    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_PRESS,
            .window = application,
            .x = 15,
            .y = 80,
            .button = 1
        },
        &widget_event));
    assert(widget_event.type == X_WIDGET_EVENT_CHANGED);
    assert(widget_event.value == 0);
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_POINTER_MOTION,
            .window = application,
            .x = 105,
            .y = 80
        },
        &widget_event));
    assert(widget_event.type == X_WIDGET_EVENT_CHANGED);
    assert(XWidgetGetValue(slider, &widget_value) && widget_value == 100);
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_RELEASE,
            .window = application,
            .x = 105,
            .y = 80,
            .button = 1
        },
        &widget_event));

    assert(XWidgetSetValue(progress, 75));
    assert(XWidgetGetValue(progress, &widget_value) && widget_value == 75);
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_BUTTON_PRESS,
            .window = application,
            .x = 20,
            .y = 130,
            .button = 1
        },
        &widget_event));
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_KEY_PRESS,
            .window = application,
            .character = '!'
        },
        &widget_event));
    assert(widget_event.type == X_WIDGET_EVENT_CHANGED);
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_KEY_PRESS,
            .window = application,
            .character = '\b'
        },
        &widget_event));
    char field_text[XLIB_TEXT_MAX];
    assert(XWidgetGetText(text_field, field_text, sizeof(field_text)));
    assert(strcmp(field_text, "hi") == 0);
    assert(XWidgetSetHandleEvent(
        widgets,
        &(XEvent){
            .type = X_EVENT_KEY_PRESS,
            .window = application,
            .character = '\n'
        },
        &widget_event));
    assert(widget_event.type == X_WIDGET_EVENT_SUBMITTED);

    for (uint32_t i = 0; i < 70; i++)
        assert(XWidgetSetDraw(widgets));
    assert(XFlush(display));
    XWidgetSetDestroy(widgets);

    assert(XSetInputFocus(display, application));
    assert(XIconifyWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.minimized && !attributes.mapped && !attributes.focused);
    assert(XRestoreWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(!attributes.minimized && attributes.mapped && attributes.focused);

    assert(XMaximizeWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.maximized && attributes.width == TEST_WIDTH);
    assert(XMaximizeWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(!attributes.maximized && attributes.width == 200);
    assert(XSetWorkArea(display, 8, 30, TEST_WIDTH - 16, TEST_HEIGHT - 40));
    assert(XMaximizeWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.maximized && attributes.x == 8 && attributes.y == 30);
    assert(attributes.width == TEST_WIDTH - 16);
    assert(attributes.height == TEST_HEIGHT - 40 - 24);
    assert(XMaximizeWindow(display, application));
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(!attributes.maximized && attributes.width == 200);
    assert(XSetWorkArea(display, 0, 0, TEST_WIDTH, TEST_HEIGHT));
    assert(!XMoveResizeWindow(display, application, 0, 0, 80, 60));
    assert(XMoveWindow(display, application, 20, 30));
    discard_events(display);

    push_mouse(-120, -80, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);
    push_mouse(0, 0, 1);
    assert(XPollEvent(display, &event));
    discard_events(display);
    push_mouse(20, 20, 1);
    assert(XPollEvent(display, &event));
    discard_events(display);
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.x == 40 && attributes.y == 50);
    push_mouse(0, 0, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);

    push_mouse(179, 40, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);
    push_mouse(0, 0, 1);
    assert(XPollEvent(display, &event));
    discard_events(display);
    push_mouse(20, 0, 1);
    assert(XPollEvent(display, &event));
    discard_events(display);
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.width == 220 && attributes.height == 150);
    push_mouse(0, 0, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);

    assert(XWMSetFeatures(display, 0));
    push_mouse(-269, 130, 0);
    assert(XPollEvent(display, &event));
    assert(event.type == X_EVENT_POINTER_MOTION);
    assert(event.window == taskbar);
    discard_events(display);

    push_mouse(0, 0, 1);
    assert(XPollEvent(display, &event));
    assert(event.type == X_EVENT_BUTTON_PRESS);
    assert(event.window == taskbar);
    assert(XGetWindowAttributes(display, taskbar, &attributes));
    assert(!attributes.focused);
    discard_events(display);

    assert(XUnmapWindow(display, application));
    assert(XUnmapWindow(display, taskbar));
    discard_events(display);
    push_mouse(0, 0, 0);
    assert(XPollEvent(display, &event));
    assert(event.type == X_EVENT_BUTTON_RELEASE);
    assert(event.window == root);
    discard_events(display);

    keyboard_events[keyboard_event_count++] = (os_keyboard_event_t){
        .scancode = 30,
        .character = 'a',
        .pressed = 1
    };
    assert(XPollEvent(display, &event));
    assert(event.type == X_EVENT_KEY_PRESS);
    assert(event.window == root && event.character == 'a');

    XWMDecorationTheme decoration_theme = {
        .active_title = XRGB(0, 174, 151),
        .inactive_title = XRGB(0, 133, 119),
        .title_text = XRGB(245, 255, 252),
        .border = XRGB(0, 72, 66),
        .button_background = XRGB(0, 151, 132),
        .button_foreground = XRGB(245, 255, 252),
        .close_background = XRGB(205, 72, 75)
    };
    assert(XWMSetFeatures(
        display,
        X_WM_DECORATIONS | X_WM_DRAG_WINDOWS |
            X_WM_AUTO_FOCUS | X_WM_AUTO_RAISE |
            X_WM_RESIZE_WINDOWS));
    assert(XWMSetDecorationTheme(display, &decoration_theme));
    assert(XMapWindow(display, application));
    discard_events(display);
    push_mouse(190, -168, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);
    push_mouse(0, 0, 1);
    assert(XPollEvent(display, &event));
    discard_events(display);
    assert(XGetWindowAttributes(display, application, &attributes));
    assert(attributes.mapped && !attributes.minimized);
    push_mouse(0, 0, 0);
    assert(XPollEvent(display, &event));
    discard_events(display);

    assert(XSetCursor(display, X_CURSOR_ARROW));

    assert(XDestroyWindow(display, taskbar));
    assert(XDestroyWindow(display, application));
    XCloseDisplay(display);
    return 0;
}
