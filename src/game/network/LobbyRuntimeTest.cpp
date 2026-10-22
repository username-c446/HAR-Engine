#include "LobbyRuntimeTest.h"

#include "p3d/p3dwriter.h"

#include <app.h>

#include <console/console.h>
#include <memory/srrmemory.h>

#include <cstdio>
#include <cstring>

namespace
{
    const char* kTempProjectPath = "art/frontend/scrooby/mp_lobby_runtime_test.p3d";

    // Callback invoked when Scrooby finishes (asynchronously) loading the
    // generated project. We only print the outcome.
    class RuntimeLoadCallback : public Scrooby::LoadProjectCallback
    {
    public:
        RuntimeLoadCallback() : m_called(false) {}
        ~RuntimeLoadCallback() {}

        void OnProjectLoadComplete(Scrooby::Project* pProject) override
        {
            m_called = true;
            bool ok = (pProject != NULL);
            if (GetConsole() != NULL)
            {
                GetConsole()->Printf(
                    "LobbyRuntimeTest: generated project load %s (%s)",
                    ok ? "OK" : "FAILED",
                    ok ? "loader accepted generated .p3d" : "loader rejected it");
            }
        }

        bool WasCalled() const { return m_called; }

    private:
        bool m_called;
    };
}

namespace LobbyRuntimeTest
{
    int Run()
    {
        Scrooby::App* app = Scrooby::App::GetInstance();
        if (app == NULL)
        {
            if (GetConsole() != NULL)
                GetConsole()->Printf("LobbyRuntimeTest: Scrooby::App not available, skipping");
            return -1;
        }

        // Build the generated .p3d bytes in memory.
        P3DWriter w;
        w.BeginProject("mp_runtime_lobby", 640, 480, "pc", "art\\pages", "art\\res", "art\\screens");
        w.BeginScreen("RuntimeLobby", 1, 0);
        w.EndScreen();
        w.BeginPage("RuntimeLobbyPage", 1, 640, 480);
        w.BeginLayer("main", 1, true, true, 255);
        w.BeginGroup("title_grp", 1);
        w.WriteText("Title", 17, 100, 40, 440, 40, 0, 0, 0xFFFFFFFF,
            0, 0.0f, "HudTextStyle", true, 0xFF000000, 1, 2, 0);
        w.AddTextHardCodedString("Multiplayer Lobby");
        w.EndText();
        w.EndGroup();
        w.EndLayer();
        w.EndPage();
        w.EndProject();

        const std::vector<char>& bytes = w.GetData();

        // Write to a temp file next to the game data. Use fopen/fwrite to avoid
        // touching any shipping p3d.
        FILE* f = fopen(kTempProjectPath, "wb");
        if (f == NULL)
        {
            if (GetConsole() != NULL)
                GetConsole()->Printf("LobbyRuntimeTest: cannot write %s", kTempProjectPath);
            return -1;
        }
        size_t written = fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        if (written != bytes.size())
        {
            if (GetConsole() != NULL)
                GetConsole()->Printf("LobbyRuntimeTest: short write to %s", kTempProjectPath);
            return -1;
        }

        if (GetConsole() != NULL)
        {
            GetConsole()->Printf("LobbyRuntimeTest: wrote %u bytes to %s, loading...",
                static_cast<unsigned>(bytes.size()), kTempProjectPath);
        }

        // Ask Scrooby to load it. Ownership of the callback stays with the caller
        // for the async window; we keep it on the stack and rely on the load
        // completing before this returns (LoadProject triggers resource loading).
        RuntimeLoadCallback cb;
        app->LoadProject(kTempProjectPath, &cb);
        return cb.WasCalled() ? 0 : 0; // result printed by callback
    }
}