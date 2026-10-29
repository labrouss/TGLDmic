#include "TGLDmicUserClient.hpp"
#include "TGLDmicShared.h"
#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(TGLDmicUserClient, IOUserClient)

const IOExternalMethodDispatch TGLDmicUserClient::sMethods[kTGLDmicMethodCount] = {
    // function       scalarIn structIn scalarOut structOut
    { &TGLDmicUserClient::sGetInfo,     0, 0, 6, 0 },
    { &TGLDmicUserClient::sStart,       0, 0, 0, 0 },
    { &TGLDmicUserClient::sStop,        0, 0, 0, 0 },
    { &TGLDmicUserClient::sGetPosition, 0, 0, 3, 0 },
};

bool TGLDmicUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) {
    if (type != kTGLDmicUserClientType) return false;
    if (!super::initWithTask(owningTask, securityToken, type, properties)) return false;
    task = owningTask;
    return true;
}

bool TGLDmicUserClient::start(IOService *provider) {
    owner = OSDynamicCast(TGLDmic, provider);
    if (!owner || !super::start(provider)) return false;
    owner->retain();
    return true;
}

void TGLDmicUserClient::stop(IOService *provider) {
    if (started && owner) { owner->clientStop(); started = false; }
    super::stop(provider);
}

IOReturn TGLDmicUserClient::clientClose() {
    if (started && owner) { owner->clientStop(); started = false; }
    OSSafeReleaseNULL(owner);
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

void TGLDmicUserClient::free() {
    OSSafeReleaseNULL(owner);
    super::free();
}

IOReturn TGLDmicUserClient::clientDied() {
    return clientClose();
}

IOReturn TGLDmicUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                           IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    if (selector >= kTGLDmicMethodCount) return kIOReturnUnsupported;
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    return super::externalMethod(selector, args, dispatch, this, reference);
}

IOReturn TGLDmicUserClient::sGetInfo(OSObject *target, void *, IOExternalMethodArguments *args) {
    auto self = OSDynamicCast(TGLDmicUserClient, target);
    if (!self || !self->owner) return kIOReturnNotAttached;
    args->scalarOutput[0] = kTGLDmicInterfaceVersion;
    args->scalarOutput[1] = self->owner->stateBits();
    args->scalarOutput[2] = TGLDmic::kRate;
    args->scalarOutput[3] = TGLDmic::kChannels;
    args->scalarOutput[4] = TGLDmic::kRingBytes;
    args->scalarOutput[5] = TGLDmic::kFrameBytes;
    return kIOReturnSuccess;
}

IOReturn TGLDmicUserClient::sStart(OSObject *target, void *, IOExternalMethodArguments *) {
    auto self = OSDynamicCast(TGLDmicUserClient, target);
    if (!self || !self->owner) return kIOReturnNotAttached;
    if (self->started) return kIOReturnSuccess;
    IOReturn r = self->owner->clientStart();
    if (r == kIOReturnSuccess) self->started = true;
    return r;
}

IOReturn TGLDmicUserClient::sStop(OSObject *target, void *, IOExternalMethodArguments *) {
    auto self = OSDynamicCast(TGLDmicUserClient, target);
    if (!self || !self->owner) return kIOReturnNotAttached;
    if (!self->started) return kIOReturnSuccess;
    self->started = false;
    return self->owner->clientStop();
}

IOReturn TGLDmicUserClient::sGetPosition(OSObject *target, void *, IOExternalMethodArguments *args) {
    auto self = OSDynamicCast(TGLDmicUserClient, target);
    if (!self || !self->owner) return kIOReturnNotAttached;
    uint64_t pos = 0, host = 0, gen = 0;
    IOReturn r = self->owner->readPosition(&pos, &host, &gen);
    args->scalarOutput[0] = pos;
    args->scalarOutput[1] = host;
    args->scalarOutput[2] = gen;
    return r;
}

IOReturn TGLDmicUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (type != kTGLDmicRingMemoryType || !owner) return kIOReturnBadArgument;
    IOMemoryDescriptor *md = owner->ringMemory();      // retained; IOUserClient releases it
    if (!md) return kIOReturnNoMemory;
    *options = kIOMapReadOnly;
    *memory = md;
    return kIOReturnSuccess;
}
