set(SOURCES WinSystemAmlogic.cpp
            VideoSyncAML.cpp
            AMLDisplay.cpp)

set(HEADERS WinSystemAmlogic.h
            VideoSyncAML.h
            AMLDisplay.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemAmlogicGLESContext.cpp
                      GraphicsPlaneAML.cpp)
  list(APPEND HEADERS WinSystemAmlogicGLESContext.h
                      GraphicsPlaneAML.h)
endif()

core_add_library(windowing_Amlogic)
