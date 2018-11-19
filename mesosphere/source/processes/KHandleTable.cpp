#include <mutex>
#include <algorithm>
#include <mesosphere/processes/KHandleTable.hpp>
#include <mesosphere/threading/KThread.hpp>
#include <mesosphere/processes/KProcess.hpp>

namespace mesosphere
{

bool KHandleTable::IsValid(Handle handle) const
{
    // Official kernel checks for nullptr, however this makes the deferred-init logic more difficult.
    // We use our own, more secure, logic instead, that is, free entries are <= 0.
    return handle.index < capacity && handle.id > 0 && entries[handle.index].id == handle.id;
}

SharedPtr<KAutoObject> KHandleTable::GetAutoObject(Handle handle) const
{
    if (!handle.IsAliasOrFree()) {
        // Note: official kernel locks the spinlock here, but we don't need to.
        return nullptr;
    } else {
        std::scoped_lock guard{spinlock};
        return IsValid(handle) ? entries[handle.index].object : nullptr;
    }
}

SharedPtr<KThread> KHandleTable::GetThread(Handle handle, bool allowAlias) const
{
    if (allowAlias && handle == selfThreadAlias) {
        return KCoreContext::GetCurrentInstance().GetCurrentThread();
    } else {
        return DynamicObjectCast<KThread>(GetAutoObject(handle));
    }
}

SharedPtr<KProcess> KHandleTable::GetProcess(Handle handle, bool allowAlias) const
{
    if (allowAlias && handle == selfProcessAlias) {
        return KCoreContext::GetCurrentInstance().GetCurrentProcess();
    } else {
        return DynamicObjectCast<KProcess>(GetAutoObject(handle));
    }
}

bool KHandleTable::Close(Handle handle)
{
    SharedPtr<KAutoObject> tmp{nullptr}; // ensure any potential dtor is called w/o the spinlock being held

    if (handle.IsAliasOrFree()) {
        return false;
    } else {
        std::scoped_lock guard{spinlock};
        if (IsValid(handle)) {
            entries[-firstFreeIndex].id = firstFreeIndex;
            firstFreeIndex = -(s16)handle.index;
            --numActive;
            tmp = std::move(entries[handle.index].object);
            return true;
        } else {
            return false;
        }
    }
}

std::tuple<Result, Handle> KHandleTable::Generate(SharedPtr<KAutoObject> obj)
{
    // Note: nullptr is accepted, for deferred-init.

    std::scoped_lock guard{spinlock};
    if (numActive >= capacity) {
        return {ResultKernelOutOfHandles(), Handle{}};
    }

    // Get/allocate the entry
    u16 index = (u16)-firstFreeIndex;
    Entry *e = &entries[-firstFreeIndex];
    firstFreeIndex = e->id;

    e->id = idCounter;
    e->object = std::move(obj);

    size = ++numActive > size ? numActive : size;
    idCounter = idCounter == 0x7FFF ? 1 : idCounter + 1;

    return {ResultSuccess(), Handle{index, e->id, false}};
}

Result KHandleTable::Set(SharedPtr<KAutoObject> obj, Handle handle)
{
    if (!handle.IsAliasOrFree() && IsValid(handle)) {
        std::scoped_lock guard{spinlock};
        entries[handle.index].object = std::move(obj);
        return ResultSuccess();
    } else {
        return ResultKernelInvalidHandle();
    }
}

void KHandleTable::Destroy()
{
    spinlock.lock();
    u16 capa = capacity;
    capacity = 0;
    firstFreeIndex = 0;
    spinlock.unlock();

    for (u16 i = 0; i < capa; i++) {
        entries[i].object = nullptr;
        entries[i].id = -(i + 1);
    }
}

/*
KHandleTable::KHandleTable(size_t capacity_) : capacity((u16)capacity_)
{
    // Note: caller should check the > case, and return an error in that case!
    capacity = capacity > capacityLimit || capacity == 0 ? (u16)capacityLimit : capacity;

    u16 capa = capacity;
    Destroy();
    capacity = capa;
}*/

KHandleTable::~KHandleTable()
{
    Destroy();
}

}