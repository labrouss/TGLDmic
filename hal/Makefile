# TGLDmic.driver (Core Audio HAL plug-in) + tgldmic-probe. Build on macOS.
OUT    = build/TGLDmic.driver
ARCH   = -arch x86_64 -mmacosx-version-min=10.15
CXXFLAGS = $(ARCH) -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
FRAMEWORKS = -framework CoreAudio -framework CoreFoundation -framework IOKit

all: $(OUT) build/tgldmic-probe

$(OUT): TGLDmicDriver.cpp Info.plist ../kext/TGLDmic/TGLDmicShared.h
	rm -rf $(OUT) && mkdir -p $(OUT)/Contents/MacOS
	clang++ $(CXXFLAGS) -bundle -o $(OUT)/Contents/MacOS/TGLDmic TGLDmicDriver.cpp $(FRAMEWORKS)
	cp Info.plist $(OUT)/Contents/Info.plist
	codesign -f -s - $(OUT) || true
	@echo "built $(OUT)"; nm -gU $(OUT)/Contents/MacOS/TGLDmic | grep TGLDmic_Create

build/tgldmic-probe: tgldmic-probe.c ../kext/TGLDmic/TGLDmicShared.h
	@mkdir -p build
	clang $(ARCH) -std=c11 -O2 -Wall -o $@ tgldmic-probe.c -framework IOKit
	codesign -f -s - $@ || true

clean:
	rm -rf build
