//
//  Object.c
//  Emojicode
//
//  Created by Theo Weidmann on 01.03.15.
//  Copyright (c) 2015 Theo Weidmann. All rights reserved.
//

#include "Memory.hpp"
#include "Class.hpp"
#include "Engine.hpp"
#include "Thread.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>

namespace Emojicode {

std::atomic_size_t memoryUse(0);
bool zeroingNeeded = false;
Byte *currentHeap;
Byte *otherHeap;

Object **deinitializationList;
std::atomic_size_t deinitializationListIndex(0);
std::atomic_size_t deinitializationListSize(7);
std::mutex deinitializationListResizeMutex;

void gc(std::unique_lock<std::mutex> &garbageCollectionLock, size_t minSpace);

size_t gcThreshold = heapSize / 2;

unsigned int pausingThreadsCount = 0;
std::atomic_bool pauseThreads(false);
std::mutex pausingThreadsCountMutex;
std::mutex garbageCollectionMutex;
std::condition_variable pauseThreadsCondition;
std::condition_variable pausingThreadsCountCondition;

inline Object* allocateObject(size_t size, Object **keep = nullptr, Thread *thread = nullptr) {
    RetainedObjectPointer rop(nullptr);
    if (pauseThreads) {
        if (keep != nullptr) {
            rop = thread->retain(*keep);
        }
        performPauseForGC();
        if (keep != nullptr) {
            *keep = rop.unretainedPointer();
            thread->release(1);
        }
    }

    size_t index;
    if ((index = memoryUse.fetch_add(size)) + size > gcThreshold) {
        memoryUse -= size;
        if (keep != nullptr) {
            rop = thread->retain(*keep);
        }
        std::unique_lock<std::mutex> lock(garbageCollectionMutex, std::try_to_lock);
        if (lock.owns_lock()) {  // OK, this thread is now the garbage collector
            gc(lock, size);
        }
        else {  // This thread also detected it’s time for garbage collection but lost the race...
            while (!pauseThreads);
            performPauseForGC();
        }
        if (keep != nullptr) {
            *keep = rop.unretainedPointer();
            thread->release(1);
        }
        return allocateObject(size);
    }
    return reinterpret_cast<Object *>(currentHeap + index);
}

inline bool inCurrentHeap(Object *o) {
    return currentHeap <= reinterpret_cast<Byte *>(o) && reinterpret_cast<Byte *>(o) < currentHeap + heapSize / 2;
}

Object* resizeObject(Object *ptr, size_t newSize, Thread *thread) {
//    auto expectation = reinterpret_cast<size_t>(ptr) - reinterpret_cast<size_t>(currentHeap);
//    size_t index = memoryUse;
//    if (index + newSize <= gcThreshold && memoryUse.compare_exchange_weak(expectation, index + newSize)) {
//        // memoryUse equaled the expectation, therefore no allocation has happend in the meantime, index still
//        // represented the value of memory use and it was leigtimate to replace memoryUse’s value with index + newSize
//        return ptr;
//    }

    Object *block = allocateObject(newSize, &ptr, thread);
    std::memcpy(block, ptr, ptr->size);
    return block;
}

Object* newObject(Class *klass) {
    Object *object = allocateObject(klass->size);
    object->size = klass->size;
    object->klass = klass;
    return object;
}

size_t sizeCalculationWithOverflowProtection(size_t items, size_t itemSize) {
    size_t r = items * itemSize;
    if (r / items != itemSize) {
        error("Integer overflow while allocating memory. It’s not possible to allocate objects of this size due to "
              "hardware limitations.");
    }
    return r;
}

Object* newArray(size_t size) {
    size_t fullSize = alignSize(sizeof(Object) + size);
    Object *object = allocateObject(fullSize);
    object->size = fullSize;
    object->klass = CL_ARRAY;
    return object;
}

Object* resizeArray(Object *array, size_t size, Thread *thread) {
    size_t fullSize = alignSize(sizeof(Object) + size);
    Object *object = resizeObject(array, fullSize, thread);
    object->size = fullSize;
    return object;
}

void registerForDeinitialization(Object *object) {
    if (deinitializationListSize == deinitializationListIndex) {
        std::lock_guard<std::mutex> lock(deinitializationListResizeMutex);
        if (deinitializationListSize == deinitializationListIndex) {
            auto newList = new Object*[deinitializationListSize * 2];
            std::memcpy(newList, deinitializationList, sizeof(Object*) * deinitializationListIndex);
            deinitializationList = newList;
            deinitializationListSize = deinitializationListSize * 2;
        }
    }
    deinitializationList[deinitializationListIndex++] = object;
}

void allocateHeap() {
    currentHeap = static_cast<Byte *>(calloc(heapSize, 1));
    if (currentHeap == nullptr) {
        error("Cannot allocate heap!");
    }
    otherHeap = currentHeap + (heapSize / 2);
    deinitializationList = new Object*[7];
}

void mark(Object **oPointer) {
    Object *oldObject = *oPointer;
    if (inCurrentHeap(oldObject->newLocation)) {
        *oPointer = oldObject->newLocation;
        return;
    }

    auto *newObject = reinterpret_cast<Object *>(currentHeap + memoryUse);
    memoryUse += oldObject->size;

    std::memcpy(newObject, oldObject, oldObject->size);

    oldObject->newLocation = newObject;
    *oPointer = newObject;
}

inline bool inOldHeap(Value *o) {
    return otherHeap <= reinterpret_cast<Byte *>(o) && reinterpret_cast<Byte *>(o) < otherHeap + heapSize / 2;
}

void markValueReference(Value **valuePointer) {
    if (!inOldHeap(*valuePointer)) {
        return;
    }
    auto b = reinterpret_cast<Byte *>(*valuePointer);

    Byte *byte = otherHeap;
    while (true) {
        auto object = reinterpret_cast<Object *>(byte);
        if (b < byte + object->size) {
            auto offset = b - byte;
            mark(&object);
            *valuePointer = reinterpret_cast<Value *>(reinterpret_cast<Byte *>(object) + offset);
            return;
        }
        byte += object->size;
    }
}

void markBox(Box *box) {
    if (box->type.raw == T_OBJECT) {
        mark(&box->value1.object);
    }
    else if ((box->type.raw & REMOTE_MASK) != 0) {
        mark(&box->value1.object);
        auto bvr = boxObjectVariableRecordTable[(box->type.raw & ~REMOTE_MASK)];
        for (size_t i = 0; i < bvr.count; i++) {
            markByObjectVariableRecord(bvr.records[i], box->value1.object->val<Value>(), i);
        }
    }
    else {
        auto bvr = boxObjectVariableRecordTable[box->type.raw];
        for (size_t i = 0; i < bvr.count; i++) {
            markByObjectVariableRecord(bvr.records[i], &box->value1, i);
        }
    }
}

void gc(std::unique_lock<std::mutex> &garbageCollectionLock, size_t minSpace) {
    pauseThreads = true;
    if (minSpace > gcThreshold) {
        error("Allocation of %zu bytes is too big. Try to enlarge the heap. (Heap size: %zu)", minSpace, heapSize);
    }

    auto pausingThreadsCountLock = std::unique_lock<std::mutex>(pausingThreadsCountMutex);
    pausingThreadsCount++;

    pausingThreadsCountCondition.wait(pausingThreadsCountLock, []{
        return pausingThreadsCount == ThreadsManager::threadsCount();
    });

    std::swap(currentHeap, otherHeap);

    size_t oldMemoryUse = memoryUse;
    memoryUse = 0;

    std::lock_guard<std::mutex> threadListLock(ThreadsManager::threadListMutex);
    for (Thread *thread = ThreadsManager::anyThread(); thread != nullptr; thread = ThreadsManager::nextThread(thread)) {
        thread->markStack();
        thread->markRetainList();
    }

    for (uint_fast16_t i = 0; i < stringPoolCount; i++) {
        mark(stringPool + i);
    }

    for (Byte *byte = currentHeap; byte < currentHeap + memoryUse;) {
        auto object = reinterpret_cast<Object *>(byte);

        for (size_t i = 0; i < object->klass->instanceVariableRecordsCount; i++) {
            auto record = object->klass->instanceVariableRecords[i];
            markByObjectVariableRecord(record, object->variableDestination(0), i);
        }

        if (object->klass->mark != nullptr) {
            object->klass->mark(object);
        }
        byte += object->size;
    }

    if (oldMemoryUse == memoryUse) {
        error("Terminating program due to too high memory pressure.");
    }

//    std::memset(otherHeap, 0xAA, heapSize / 2);

    if (zeroingNeeded) {
        std::memset(currentHeap + memoryUse, 0, (heapSize / 2) - memoryUse);
    }
    else {
        zeroingNeeded = true;
    }

    size_t place = 0;
    for (size_t i = 0; i < deinitializationListIndex; i++) {
        if (inCurrentHeap(deinitializationList[i]->newLocation)) {
            deinitializationList[place++] = deinitializationList[i]->newLocation;
        }
        else {
            deinitializationList[i]->klass->deinit(deinitializationList[i]);
        }
    }
    deinitializationListIndex = place;

    pausingThreadsCount--;
    pauseThreads = false;
    garbageCollectionLock.unlock();

    pauseThreadsCondition.notify_all();
    pausingThreadsCountLock.unlock();
}

void pauseForGC() {
    if (pauseThreads) {
        performPauseForGC();
    }
}

inline void performPauseForGC() {
    auto pausingThreadsCountLock = std::unique_lock<std::mutex>(pausingThreadsCountMutex);
    pausingThreadsCount++;
    pausingThreadsCountCondition.notify_one();
    pauseThreadsCondition.wait(pausingThreadsCountLock, []{ return !pauseThreads; });
    pausingThreadsCount--;
}

void allowGC() {
    std::unique_lock<std::mutex> pausingThreadsCountLock(pausingThreadsCountMutex);
    pausingThreadsCount++;
    pausingThreadsCountCondition.notify_one();
}

void disallowGCAndPauseIfNeeded() {
    auto pausingThreadsCountLock = std::unique_lock<std::mutex>(pausingThreadsCountMutex);
    pauseThreadsCondition.wait(pausingThreadsCountLock, []{ return !pauseThreads; });
    pausingThreadsCount--;
    pausingThreadsCountCondition.notify_one();
}

}  // namespace Emojicode
