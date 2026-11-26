#ifndef VERSITILE_LIBX_XWIDGETS_H
#define VERSITILE_LIBX_XWIDGETS_H

#include <stdbool.h>
#include <stdint.h>

#include <X11/Xlib.h>

#define X_WIDGET_MAX 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XWidgetSet XWidgetSet;
typedef struct XWidget XWidget;

typedef enum
{
    X_WIDGET_BUTTON = 0,
    X_WIDGET_LABEL,
    X_WIDGET_CHECKBOX,
    X_WIDGET_SLIDER,
    X_WIDGET_PROGRESS,
    X_WIDGET_TEXT_FIELD
} XWidgetType;

typedef enum
{
    X_WIDGET_EVENT_NONE = 0,
    X_WIDGET_EVENT_ACTIVATED,
    X_WIDGET_EVENT_CHANGED,
    X_WIDGET_EVENT_SUBMITTED
} XWidgetEventType;

typedef struct
{
    uint32_t background;
    uint32_t foreground;
    uint32_t accent;
    uint32_t border;
    uint32_t disabled;
} XWidgetTheme;

typedef struct
{
    XWidgetEventType type;
    uint32_t id;
    uint32_t value;
    char text[XLIB_TEXT_MAX];
} XWidgetEvent;

XWidgetSet *XWidgetSetCreate(Display *display, Window *window);
void XWidgetSetDestroy(XWidgetSet *set);
bool XWidgetSetSetTheme(XWidgetSet *set, const XWidgetTheme *theme);
bool XWidgetSetDraw(XWidgetSet *set);
bool XWidgetSetHandleEvent(
    XWidgetSet *set,
    const XEvent *event,
    XWidgetEvent *widget_event);

XWidget *XWidgetAddButton(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const char *label);
XWidget *XWidgetAddLabel(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const char *text);
XWidget *XWidgetAddCheckbox(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    const char *label,
    bool checked);
XWidget *XWidgetAddSlider(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t value);
XWidget *XWidgetAddProgressBar(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t value);
XWidget *XWidgetAddTextField(
    XWidgetSet *set,
    uint32_t id,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t maximum_length,
    const char *text);

bool XWidgetSetEnabled(XWidget *widget, bool enabled);
bool XWidgetSetValue(XWidget *widget, uint32_t value);
bool XWidgetGetValue(const XWidget *widget, uint32_t *value);
bool XWidgetSetText(XWidget *widget, const char *text);
bool XWidgetGetText(
    const XWidget *widget,
    char *text,
    uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
