#include "textinputhook.h"

// Singleton-owned text input hook. Kept simple and global: the UI overlay that
// needs text input registers itself as the active consumer, the platform event
// loop forwards SDL_TEXTINPUT events through Report*.
namespace
{
    TextInputHook::TextCallback s_textCb = NULL;
    TextInputHook::BackspaceCallback s_backspaceCb = NULL;
}

namespace TextInputHook
{
    void SetActive(TextCallback textCb, BackspaceCallback backspaceCb)
    {
        s_textCb = textCb;
        s_backspaceCb = backspaceCb;
    }

    void ClearActive()
    {
        s_textCb = NULL;
        s_backspaceCb = NULL;
    }

    void ReportText(const char* utf8Char)
    {
        if (s_textCb && utf8Char && utf8Char[0] != '\0')
        {
            s_textCb(utf8Char);
        }
    }

    void ReportBackspace()
    {
        if (s_backspaceCb)
        {
            s_backspaceCb();
        }
    }

    bool HasActiveConsumer()
    {
        return s_textCb != NULL;
    }
}