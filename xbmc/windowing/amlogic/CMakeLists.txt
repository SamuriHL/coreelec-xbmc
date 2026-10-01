set(SOURCES WinSystemAmlogic.cpp
            VideoSyncAML.cpp
            VideoSyncTimeline.cpp
            AMLDisplay.cpp
            Timekeeper.cpp
            AudioFollower.cpp)

set(HEADERS WinSystemAmlogic.h
            VideoSyncAML.h
            VideoSyncTimeline.h
            AMLDisplay.h
            Timekeeper.h
            AudioFollower.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemAmlogicGLESContext.cpp
                      GraphicsPlaneAML.cpp)
  list(APPEND HEADERS WinSystemAmlogicGLESContext.h
                      GraphicsPlaneAML.h)
endif()

core_add_library(windowing_Amlogic)
