TARGET = LiwaSandbox
OBJS   = main.o

CFLAGS   = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR =
LIBS   = -lpspgum -lpspgu -lpspvfpu -lpspdebug -lpspctrl -lpspdisplay -lpspge -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = Liwa Sandbox
PSP_FW_VERSION  = 500
BUILD_PRX       = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
