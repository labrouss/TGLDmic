#pragma once
#include <IOKit/IOUserClient.h>
#include "TGLDmic.hpp"

// User client for the Core Audio HAL plug-in (runs inside coreaudiod's driver service).
// Each open client may call Start once; Stop or closing the connection releases it.
class TGLDmicUserClient : public IOUserClient {
    OSDeclareDefaultStructors(TGLDmicUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    TGLDmic *owner {nullptr};
    task_t task {nullptr};
    bool started {false};

    static IOReturn sGetInfo(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sStart(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sStop(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static IOReturn sGetPosition(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static const IOExternalMethodDispatch sMethods[];
};
