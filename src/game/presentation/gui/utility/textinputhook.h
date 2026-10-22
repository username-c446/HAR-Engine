#pragma once

// Cross-platform text-input hook for SDL_TEXTINPUT events. The game does not
// route keyboard text into GUI screens by default; this tiny bridge lets a
// screen (e.g. the multiplayer IP entry overlay) receive typed characters
// without touching the platform event loop's platform-specific parts.
//
// The SDL event loop calls ReportText()/ReportBackspace() when the app is
// running; the currently registered callback (set by an active overlay) receives
// the characters. Only one consumer at a time.
namespace TextInputHook
{
    // Receives printable characters as they are typed (UTF-8 rune per call).
    typedef void (*TextCallback)(const char* utf8Char);
    // Receives backspace presses (to delete the last character).
    typedef void (*BackspaceCallback)();

    void SetActive(TextCallback textCb, BackspaceCallback backspaceCb);
    void ClearActive();

    void ReportText(const char* utf8Char);
    void ReportBackspace();

    bool HasActiveConsumer();
}