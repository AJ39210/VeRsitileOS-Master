#include <assert.h>
#include <stdint.h>

#include <X11/Xdesktop.h>
#include <X11/Xwidgets.h>

void yield(void)
{
}

int main(void)
{
    assert(XRGB(0x12, 0x34, 0x56) == 0x00123456U);
    assert(X_MASK_KEY_PRESS != X_MASK_KEY_RELEASE);
    assert(X_MASK_ALL ==
           (X_MASK_KEY_PRESS | X_MASK_KEY_RELEASE |
            X_MASK_BUTTON_PRESS | X_MASK_BUTTON_RELEASE |
            X_MASK_POINTER_MOTION | X_MASK_FOCUS | X_MASK_CLOSE |
            X_MASK_CONFIGURE | X_MASK_WINDOW_STATE | X_MASK_WORKSPACE));
    assert(X_CURSOR_HIDDEN > X_CURSOR_CROSSHAIR);
    assert(X_WIDGET_TEXT_FIELD > X_WIDGET_PROGRESS);
    assert(X_WIDGET_EVENT_SUBMITTED > X_WIDGET_EVENT_CHANGED);
    return 0;
}
