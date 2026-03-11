#include <android_native_app_glue.h>

#include "quest_passthrough_app.hpp"

void android_main(android_app* app) {
    app_dummy();

    QuestPassthroughApp xrApp(app);
    app->userData = &xrApp;
    app->onAppCmd = [](android_app* appState, int32_t cmd) {
        auto* xrAppState = static_cast<QuestPassthroughApp*>(appState->userData);
        if (xrAppState != nullptr) {
            xrAppState->HandleAppCommand(cmd);
        }
    };

    xrApp.RunMainLoop();
}
