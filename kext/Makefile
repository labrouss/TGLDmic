# Build TGLDmic.kext on macOS (Xcode CLT). make SDK=/path/to/MacKernelSDK
SDK     ?= $(CURDIR)/../MacKernelSDK
# Intel production-signed (the CSE rejects community-signed builds: ROM error 44)
FWIMAGE  = ../firmware/intel/sof-tgl.ri
OBJS     = build/TGLDmic.o build/DspBoot.o build/Capture.o build/TGLDmicUserClient.o build/Firmware.o build/kmod_info.o
OUT      = build/TGLDmic.kext
ARCH     = -arch x86_64 -mmacosx-version-min=10.15
CXXFLAGS = $(ARCH) -std=gnu++17 -O2 -Wall -Wno-unused-private-field \
           -nostdinc -isystem $(SDK)/Headers -mkernel -fapple-kext -fno-rtti -fno-exceptions \
           -fno-builtin -fno-common -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -ITGLDmic
CFLAGS   = $(ARCH) -std=gnu11 -O2 -nostdinc -isystem $(SDK)/Headers -mkernel -fno-builtin -fno-common -DKERNEL
CCKEXT   = $(shell clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a
LDFLAGS  = $(ARCH) -nostdlib -Xlinker -kext -Xlinker -export_dynamic -L$(SDK)/Library/x86_64 -lkmod \
           $(wildcard $(CCKEXT))

all: $(OUT)

build/%.o: TGLDmic/%.cpp TGLDmic/*.hpp
	@mkdir -p build
	clang++ $(CXXFLAGS) -c $< -o $@
build/Firmware.c: $(FWIMAGE) ../tools/gen-fw.py
	@mkdir -p build
	python3 ../tools/gen-fw.py $(FWIMAGE) $@
build/Firmware.o: build/Firmware.c
	clang $(CFLAGS) -c $< -o $@
build/kmod_info.o: TGLDmic/kmod_info.c
	@mkdir -p build
	clang $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS) TGLDmic/Info.plist
	rm -rf $(OUT) && mkdir -p $(OUT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(OUT)/Contents/MacOS/TGLDmic
	cp TGLDmic/Info.plist $(OUT)/Contents/Info.plist
	codesign -f -s - $(OUT) || true
	cd build && ditto -c -k --keepParent TGLDmic.kext TGLDmic.kext.zip
	@echo "built $(OUT)"; nm -gU $(OUT)/Contents/MacOS/TGLDmic | grep -E 'kmod_info|_start' || true

clean:
	rm -rf build
