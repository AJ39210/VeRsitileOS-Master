#include <stdlib.h>
#include <string.h>

#include <X11/Xdesktop.h>
#include <X11/Xwidgets.h>

#define X_WIDGET_TEXT_MAX (XLIB_TEXT_MAX - 1)
#define X_WIDGET_MIN_HEIGHT 18

struct XWidget
{
    XWidgetSet *set;
    XWidgetType type;
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t value;
    uint32_t maximum_length;
    char text[XLIB_TEXT_MAX];
    bool enabled;
    bool pressed;
};

struct XWidgetSet
{
    Display *display;
    Window *window;
    XWidget widgets[X_WIDGET_MAX];
    uint32_t count;
    XWidgetTheme theme;
    XWidget *captured;
    XWidget *focused;
};

static bool widget_text_copy(char *destination, const char *source)
{
    if (!destination || !source)
        return false;
    size_t length = strlen(source);
    if (length > X_WIDGET_TEXT_MAX)
        return false;
    memcpy(destination, source, length + 1);
    return true;
}

static bool widget_bounds_valid(
    XWidgetSet *set,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height)
{
    XWindowAttributes attributes;
    if (!set || !width || height < X_WIDGET_MIN_HEIGHT ||
        !XGetWindowAttributes(set->display, set->window, &attributes))
        return false;
    return x >= 0 && y >= 0 &&
        (int64_t)x + width <= attributes.width &&
        (int64_t)y + height <= attributes.height;
}

static XWidget *widget_add(
    XWidgetSet *set,
    XWidgetType type,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const char *text)
{
    if (!widget_bounds_valid(set, x, y, width, height) ||
        set->count == X_WIDGET_MAX ||
        (text && strlen(text) > X_WIDGET_TEXT_MAX))
        return NULL;
    for (uint32_t i = 0; i < set->count; i++)
        if (set->widgets[i].id == id)
            return NULL;

    XWidget *widget = &set->widgets[set->count];
    *widget = (XWidget){
        .set = set,
        .type = type,
        .id = id,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .enabled = true
    };
    if (text && !widget_text_copy(widget->text, text))
        return NULL;
    set->count++;
    return widget;
}

XWidgetSet *XWidgetSetCreate(Display *display, Window *window)
{
    XWindowAttributes attributes;
    if (!display || !window ||
        !XGetWindowAttributes(display, window, &attributes))
        return NULL;
    XWidgetSet *set = calloc(1, sizeof(*set));
    if (!set)
        return NULL;
    set->display = display;
    set->window = window;
    set->theme = (XWidgetTheme){
        .background = XRGB(42, 48, 60),
        .foreground = XRGB(242, 244, 248),
        .accent = XRGB(65, 130, 220),
        .border = XRGB(105, 118, 138),
        .disabled = XRGB(120, 126, 138)
    };
    return set;
}

void XWidgetSetDestroy(XWidgetSet *set)
{
    if (set && set->focused)
        (void)XSetCursor(set->display, X_CURSOR_ARROW);
    free(set);
}

bool XWidgetSetSetTheme(XWidgetSet *set, const XWidgetTheme *theme)
{
    if (!set || !theme)
        return false;
    set->theme = *theme;
    return true;
}

XWidget *XWidgetAddButton(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height, const char *label)
{
    if (!label || width < 16)
        return NULL;
    return widget_add(set, X_WIDGET_BUTTON, id, x, y, width, height, label);
}

XWidget *XWidgetAddLabel(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height, const char *text)
{
    if (!text)
        return NULL;
    return widget_add(set, X_WIDGET_LABEL, id, x, y, width, height, text);
}

XWidget *XWidgetAddCheckbox(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height, const char *label, bool checked)
{
    if (!label || !set || width < height + 8)
        return NULL;
    XWidget *widget =
        widget_add(set, X_WIDGET_CHECKBOX, id, x, y, width, height, label);
    if (widget)
        widget->value = checked ? 1 : 0;
    return widget;
}

static XWidget *widget_add_range(
    XWidgetSet *set, XWidgetType type, uint32_t id,
    int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t minimum, uint32_t maximum, uint32_t value)
{
    if (maximum <= minimum || value < minimum || value > maximum ||
        (type == X_WIDGET_SLIDER && width < 18) ||
        (type == X_WIDGET_PROGRESS && width < 3))
        return NULL;
    XWidget *widget = widget_add(
        set, type, id, x, y, width, height, NULL);
    if (widget)
    {
        widget->minimum = minimum;
        widget->maximum = maximum;
        widget->value = value;
    }
    return widget;
}

XWidget *XWidgetAddSlider(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height,
    uint32_t minimum, uint32_t maximum, uint32_t value)
{
    return widget_add_range(
        set, X_WIDGET_SLIDER, id, x, y, width, height,
        minimum, maximum, value);
}

XWidget *XWidgetAddProgressBar(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height,
    uint32_t minimum, uint32_t maximum, uint32_t value)
{
    return widget_add_range(
        set, X_WIDGET_PROGRESS, id, x, y, width, height,
        minimum, maximum, value);
}

XWidget *XWidgetAddTextField(
    XWidgetSet *set, uint32_t id, int32_t x, int32_t y,
    uint32_t width, uint32_t height,
    uint32_t maximum_length, const char *text)
{
    if (!text || width < 20 || !maximum_length ||
        maximum_length > X_WIDGET_TEXT_MAX ||
        strlen(text) > maximum_length)
        return NULL;
    XWidget *widget = widget_add(
        set, X_WIDGET_TEXT_FIELD, id, x, y, width, height, text);
    if (widget)
        widget->maximum_length = maximum_length;
    return widget;
}

bool XWidgetSetEnabled(XWidget *widget, bool enabled)
{
    if (!widget)
        return false;
    widget->enabled = enabled;
    if (!enabled && widget->set->captured == widget)
    {
        widget->set->captured = NULL;
        widget->pressed = false;
    }
    if (!enabled && widget->set->focused == widget)
    {
        widget->set->focused = NULL;
        (void)XSetCursor(widget->set->display, X_CURSOR_ARROW);
    }
    return true;
}

bool XWidgetSetValue(XWidget *widget, uint32_t value)
{
    if (!widget)
        return false;
    if (widget->type == X_WIDGET_CHECKBOX)
    {
        if (value > 1)
            return false;
    }
    else if (widget->type == X_WIDGET_SLIDER ||
             widget->type == X_WIDGET_PROGRESS)
    {
        if (value < widget->minimum || value > widget->maximum)
            return false;
    }
    else
        return false;
    widget->value = value;
    return true;
}

bool XWidgetGetValue(const XWidget *widget, uint32_t *value)
{
    if (!widget || !value ||
        (widget->type != X_WIDGET_CHECKBOX &&
         widget->type != X_WIDGET_SLIDER &&
         widget->type != X_WIDGET_PROGRESS))
        return false;
    *value = widget->value;
    return true;
}

bool XWidgetSetText(XWidget *widget, const char *text)
{
    if (!widget || !text || strlen(text) > X_WIDGET_TEXT_MAX ||
        (widget->type == X_WIDGET_TEXT_FIELD &&
         strlen(text) > widget->maximum_length))
        return false;
    return widget_text_copy(widget->text, text);
}

bool XWidgetGetText(
    const XWidget *widget, char *text, uint32_t capacity)
{
    if (!widget || !text || !capacity)
        return false;
    size_t length = strlen(widget->text);
    if (length >= capacity)
        return false;
    memcpy(text, widget->text, length + 1);
    return true;
}

static bool widget_contains(
    const XWidget *widget, int32_t x, int32_t y)
{
    return x >= widget->x && y >= widget->y &&
        (int64_t)x < (int64_t)widget->x + widget->width &&
        (int64_t)y < (int64_t)widget->y + widget->height;
}

static void widget_emit(
    XWidgetEvent *event, XWidgetEventType type, const XWidget *widget)
{
    if (!event)
        return;
    event->type = type;
    event->id = widget->id;
    event->value = widget->value;
    memcpy(event->text, widget->text, sizeof(event->text));
}

static uint32_t slider_value_at(const XWidget *widget, int32_t x)
{
    uint32_t track_width = widget->width - 10;
    int64_t local = (int64_t)x - widget->x - 5;
    if (local < 0)
        local = 0;
    if ((uint64_t)local > track_width)
        local = track_width;
    uint64_t range = widget->maximum - widget->minimum;
    uint64_t offset =
        range * (uint64_t)local / track_width;
    return widget->minimum + (uint32_t)offset;
}

bool XWidgetSetHandleEvent(
    XWidgetSet *set,
    const XEvent *event,
    XWidgetEvent *widget_event)
{
    if (!set || !event || event->window != set->window)
        return false;
    if (widget_event)
        *widget_event = (XWidgetEvent){0};

    if (event->type == X_EVENT_BUTTON_PRESS && event->button == 1)
    {
        set->focused = NULL;
        set->captured = NULL;
        (void)XSetCursor(set->display, X_CURSOR_ARROW);
        for (uint32_t i = set->count; i > 0; i--)
        {
            XWidget *widget = &set->widgets[i - 1];
            if (!widget->enabled || !widget_contains(widget, event->x, event->y))
                continue;
            if (widget->type == X_WIDGET_LABEL ||
                widget->type == X_WIDGET_PROGRESS)
                return true;
            set->captured = widget;
            if (widget->type == X_WIDGET_TEXT_FIELD)
            {
                set->focused = widget;
                (void)XSetCursor(set->display, X_CURSOR_IBEAM);
            }
            else
                widget->pressed = true;
            if (widget->type == X_WIDGET_SLIDER)
            {
                uint32_t value = slider_value_at(widget, event->x);
                if (value != widget->value)
                {
                    widget->value = value;
                    widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
                }
            }
            return true;
        }
        (void)XSetCursor(set->display, X_CURSOR_ARROW);
        return false;
    }

    if (event->type == X_EVENT_POINTER_MOTION && set->captured &&
        set->captured->type == X_WIDGET_SLIDER)
    {
        XWidget *widget = set->captured;
        uint32_t value = slider_value_at(widget, event->x);
        if (value != widget->value)
        {
            widget->value = value;
            widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
        }
        return true;
    }

    if (event->type == X_EVENT_BUTTON_RELEASE && event->button == 1 &&
        set->captured)
    {
        XWidget *widget = set->captured;
        bool inside = widget_contains(widget, event->x, event->y);
        set->captured = NULL;
        widget->pressed = false;
        if (!inside)
            return true;
        if (widget->type == X_WIDGET_BUTTON)
            widget_emit(widget_event, X_WIDGET_EVENT_ACTIVATED, widget);
        else if (widget->type == X_WIDGET_CHECKBOX)
        {
            widget->value = !widget->value;
            widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
        }
        else if (widget->type == X_WIDGET_SLIDER)
        {
            uint32_t value = slider_value_at(widget, event->x);
            if (value != widget->value)
            {
                widget->value = value;
                widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
            }
        }
        return true;
    }

    if (event->type == X_EVENT_KEY_PRESS && set->focused &&
        set->focused->enabled)
    {
        XWidget *widget = set->focused;
        size_t length = strlen(widget->text);
        if (event->character == '\b' || event->character == 127)
        {
            if (length)
            {
                widget->text[length - 1] = '\0';
                widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
            }
            return true;
        }
        if (event->character == '\r' || event->character == '\n')
        {
            widget_emit(widget_event, X_WIDGET_EVENT_SUBMITTED, widget);
            return true;
        }
        if (event->character >= 32 && event->character <= 126 &&
            length < widget->maximum_length)
        {
            widget->text[length] = (char)event->character;
            widget->text[length + 1] = '\0';
            widget_emit(widget_event, X_WIDGET_EVENT_CHANGED, widget);
            return true;
        }
    }
    return false;
}

static bool widget_draw_text(
    XWidgetSet *set,
    XWidget *widget,
    int32_t x,
    int32_t y,
    uint32_t available_width)
{
    const XWidgetTheme *theme = &set->theme;
    size_t length = strlen(widget->text);
    uint32_t max_characters = available_width / 8;
    if (length > max_characters)
        length = max_characters;
    char visible[XLIB_TEXT_MAX];
    memcpy(visible, widget->text, length);
    visible[length] = '\0';
    return XDrawString(
        set->display, set->window,
        widget->enabled ? theme->foreground : theme->disabled,
        x, y, visible);
}

static bool widget_draw_one(XWidgetSet *set, XWidget *widget)
{
    const XWidgetTheme *theme = &set->theme;
    uint32_t foreground =
        widget->enabled ? theme->foreground : theme->disabled;
    int32_t center_y = widget->y + (int32_t)widget->height / 2 - 8;
    switch (widget->type)
    {
        case X_WIDGET_BUTTON:
            if (!XFillRectangle(
                    set->display, set->window,
                    widget->pressed ? theme->accent : theme->background,
                    widget->x, widget->y, widget->width, widget->height) ||
                !XDrawRectangle(
                    set->display, set->window, theme->border,
                    widget->x, widget->y, widget->width, widget->height))
                return false;
            return widget_draw_text(
                set, widget, widget->x + 8, center_y,
                widget->width - 16);

        case X_WIDGET_LABEL:
            return widget_draw_text(
                set, widget, widget->x, center_y, widget->width);

        case X_WIDGET_CHECKBOX:
        {
            int32_t box_size = (int32_t)widget->height - 6;
            int32_t box_y = widget->y + 3;
            if (!XFillRectangle(
                    set->display, set->window,
                    theme->background, widget->x, box_y,
                    (uint32_t)box_size, (uint32_t)box_size) ||
                !XDrawRectangle(
                    set->display, set->window, theme->border,
                    widget->x, box_y,
                    (uint32_t)box_size, (uint32_t)box_size))
                return false;
            if (widget->value &&
                !XFillRectangle(
                    set->display, set->window, theme->accent,
                    widget->x + 4, box_y + 4,
                    (uint32_t)box_size - 8, (uint32_t)box_size - 8))
                return false;
            return widget_draw_text(
                set, widget, widget->x + box_size + 8, center_y,
                widget->width - (uint32_t)box_size - 8);
        }

        case X_WIDGET_SLIDER:
        {
            uint32_t track_width = widget->width - 10;
            uint32_t track_height = 4;
            int32_t track_x = widget->x + 5;
            int32_t track_y =
                widget->y + (int32_t)widget->height / 2 - 2;
            uint64_t range = widget->maximum - widget->minimum;
            uint32_t fill_width = (uint32_t)(
                (uint64_t)track_width * (widget->value - widget->minimum) /
                range);
            int32_t knob_x = track_x + (int32_t)fill_width - 4;
            int32_t knob_y = track_y - 5;
            return XFillRectangle(
                       set->display, set->window, theme->border,
                       track_x, track_y, track_width, track_height) &&
                XFillRectangle(
                    set->display, set->window, theme->accent,
                    track_x, track_y, fill_width, track_height) &&
                XFillRectangle(
                    set->display, set->window, foreground,
                    knob_x, knob_y, 8, 14);
        }

        case X_WIDGET_PROGRESS:
        {
            uint32_t inner_width = widget->width > 2
                ? widget->width - 2 : 0;
            uint32_t inner_height = widget->height > 2
                ? widget->height - 2 : 0;
            uint64_t range = widget->maximum - widget->minimum;
            uint32_t fill_width = (uint32_t)(
                (uint64_t)inner_width * (widget->value - widget->minimum) /
                range);
            return XFillRectangle(
                       set->display, set->window, theme->background,
                       widget->x, widget->y, widget->width, widget->height) &&
                XDrawRectangle(
                    set->display, set->window, theme->border,
                    widget->x, widget->y, widget->width, widget->height) &&
                XFillRectangle(
                    set->display, set->window, theme->accent,
                    widget->x + 1, widget->y + 1,
                    fill_width, inner_height);
        }

        case X_WIDGET_TEXT_FIELD:
        {
            if (!XFillRectangle(
                    set->display, set->window, theme->background,
                    widget->x, widget->y, widget->width, widget->height) ||
                !XDrawRectangle(
                    set->display, set->window,
                    set->focused == widget ? theme->accent : theme->border,
                    widget->x, widget->y, widget->width, widget->height))
                return false;
            size_t length = strlen(widget->text);
            uint32_t visible_characters = (widget->width - 12) / 8;
            size_t start = length > visible_characters
                ? length - visible_characters : 0;
            char visible[XLIB_TEXT_MAX];
            size_t visible_length = length - start;
            memcpy(visible, widget->text + start, visible_length);
            visible[visible_length] = '\0';
            if (!XDrawString(
                    set->display, set->window, foreground,
                    widget->x + 6, center_y, visible))
                return false;
            if (set->focused == widget)
            {
                int32_t caret_x = widget->x + 6 +
                    (int32_t)visible_length * 8;
                return XFillRectangle(
                    set->display, set->window, foreground,
                    caret_x, center_y, 1, 16);
            }
            return true;
        }
    }
    return false;
}

bool XWidgetSetDraw(XWidgetSet *set)
{
    if (!set)
        return false;
    for (uint32_t i = 0; i < set->count; i++)
        if (!widget_draw_one(set, &set->widgets[i]))
            return false;
    return true;
}
