set(SOURCES WinSystemAmlogic.cpp
            VideoSyncAML.cpp
            VideoSyncTimeline.cpp
            AMLDisplay.cpp
            Timekeeper.cpp
            AudioFollower.cpp
            WireClassProbe.cpp
            OutputWorker.cpp
            SettingsMirror.cpp
            ThreadRole.cpp
            DisplayState.cpp)

set(HEADERS WinSystemAmlogic.h
            VideoSyncAML.h
            VideoSyncTimeline.h
            AMLDisplay.h
            Timekeeper.h
            AudioFollower.h
            WireClassProbe.h
            OutputWorker.h
            SettingsMirror.h
            ThreadRole.h
            DisplayState.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemAmlogicGLESContext.cpp
                      GraphicsPlaneAML.cpp)
  list(APPEND HEADERS WinSystemAmlogicGLESContext.h
                      GraphicsPlaneAML.h)
endif()

core_add_library(windowing_Amlogic)
