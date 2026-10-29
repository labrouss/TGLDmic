#!/usr/bin/env python3
"""List Core Audio devices via ctypes (no Xcode needed): ID, name, UID, input/output channels.
    python3 list-audio-devices.py
"""
import ctypes, ctypes.util, struct

ca = ctypes.CDLL(ctypes.util.find_library("CoreAudio"))
cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))

def fourcc(s): return struct.unpack(">I", s.encode())[0]

class Addr(ctypes.Structure):
    _fields_ = [("mSelector", ctypes.c_uint32), ("mScope", ctypes.c_uint32), ("mElement", ctypes.c_uint32)]

ca.AudioObjectGetPropertyDataSize.argtypes = [ctypes.c_uint32, ctypes.POINTER(Addr), ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
ca.AudioObjectGetPropertyData.argtypes = [ctypes.c_uint32, ctypes.POINTER(Addr), ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
cf.CFRelease.argtypes = [ctypes.c_void_p]
GLOB, INP, OUTP = fourcc("glob"), fourcc("inpt"), fourcc("outp")

def get(obj, sel, scope=GLOB):
    a = Addr(fourcc(sel), scope, 0); n = ctypes.c_uint32(0)
    err = ca.AudioObjectGetPropertyDataSize(obj, ctypes.byref(a), 0, None, ctypes.byref(n))
    if err: return None, err
    buf = ctypes.create_string_buffer(n.value)
    err = ca.AudioObjectGetPropertyData(obj, ctypes.byref(a), 0, None, ctypes.byref(n), buf)
    return (buf.raw[:n.value] if not err else None), err

def cfstr(obj, sel):
    raw, err = get(obj, sel)
    if not raw: return f"<err {err}>"
    ref = ctypes.c_void_p(struct.unpack("<Q", raw)[0])
    out = ctypes.create_string_buffer(512)
    cf.CFStringGetCString(ref, out, 512, 0x08000100)
    cf.CFRelease(ref)
    return out.value.decode(errors="replace")

def channels(dev, scope):
    raw, err = get(dev, "slay", scope)              # kAudioDevicePropertyStreamConfiguration (AudioBufferList)
    if not raw: return 0
    n = struct.unpack("<I", raw[:4])[0]
    return sum(struct.unpack("<II", raw[8 + i * 16: 16 + i * 16])[0] for i in range(n))

raw, err = get(1, "dev#")                            # kAudioHardwarePropertyDevices on the system object
if raw is None:
    raise SystemExit(f"cannot list devices: {err}")
for (dev,) in struct.iter_unpack("<I", raw):
    print(f"[{dev:3}] {cfstr(dev, 'lnam')!r:42} in {channels(dev, INP)} out {channels(dev, OUTP)}  uid={cfstr(dev, 'uid ')}")
