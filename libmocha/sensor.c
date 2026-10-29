/*
 * Copyright (C) 2016 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LibHi6210sft[Camera]"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <log/log.h>
#include <hardware/power.h>
#include <hardware/hardware.h>

//various funcs we'll need to call, in their mangled form

//android::String8::String8(char const*)
extern void _ZN7android7String8C1EPKc(void **str8P, const char *str);

//android::String8::~String8()
extern void _ZN7android7String8D1Ev(void **str8P);

//android::String16::String16(char const*)
extern void _ZN7android8String16C1EPKc(void **str16P, const char *str);

//android::String16::~String16()
extern void _ZN7android8String16D1Ev(void **str16P);

//android::SensorManager::~SensorManager()
extern void _ZN7android13SensorManagerD1Ev(void *sensorMgr);

//android::SensorManager::SensorManager(android::String16 const&)
extern void _ZN7android13SensorManagerC1ERKNS_8String16E(void *sensorMgr, void **str16P);

//android::SensorManager::createEventQueue(android::String8, int)
//android::SensorManager::createEventQueue(const String8&, int32_t pid, const String16&)
// (Android 11+ takes a third String16 opPackageName parameter)
extern void _ZN7android13SensorManager16createEventQueueENS_7String8EiNS_8String16E(void **retVal, void *sensorMgr, void **str8P, int pid, void **str16P);


//data exports we must provide for camera library to be happy

/*
 * DATA:     android::Singleton<android::SensorManager>::sLock
 * USE:      INTERPOSE: a mutes that camera lib will insist on accessing
 * NOTES:    In L, the sensor manager exposed this lock that callers
 *           actually locked & unlocked when accessing it. In M this
 *           is no longer the case, but we still must provide it for
 *           the camera library to be happy. It will lock nothnhing, but
 *           as long as it is a real lock and pthread_mutex_* funcs
 *           work on it, the camera library will be happy.
 */
pthread_mutex_t _ZN7android9SingletonINS_13SensorManagerEE5sLockE = PTHREAD_MUTEX_INITIALIZER;

/*
 * DATA:     android::Singleton<android::SensorManager>::sInstance
 * USE:      INTERPOSE: a singleton instance of SensorManager
 * NOTES:    In L, the sensor manager exposed this variable, as it was
 *           a singleton and one could just access this directly to get
 *           the current already-existing instance if it happened to
 *           already exist. If not one would create one and store it
 *           there. In M this is entirely different, but the camera library
 *           does not know that. So we'll init it to NULL to signify that
 *           no current instance exists, let it create one, and store it
 *           here, and upon unloading we'll clean it up, if it is not
 *           NULL (which is what it would be if the camera library itself
 *           did the cleanup).
 */
void* _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = NULL;

//code exports we provide

//android::SensorManager::SensorManager(void)
void _ZN7android13SensorManagerC1Ev(void *sensorMgr);

//android::SensorManager::createEventQueue(void)
void _ZN7android13SensorManager16createEventQueueEv(void **retVal, void *sensorMgr);

//library on-load and on-unload handlers (to help us set things up and tear them down)
void libEvtLoading(void) __attribute__((constructor));
void libEvtUnloading(void) __attribute__((destructor));

/*
 * FUNCTION: android::SensorManager::SensorManager(void)
 * USE:      INTERPOSE: construct a sensor manager object
 * NOTES:    This constructor no longer exists in M, instead now one must pass
 *           in a package name as a "string16" to the consrtuctor. Since this
 *           lib only services camera library, it is easy for us to just do that
 *           and this provide the constructor that the camera library wants.
 *           El original del árbol msm8916 usaba "camera.msm8960"/"camera.msm8916".
 *
 *           2026-10-28 (medido en 19.1/mocha): ese nombre NO existe como paquete, y en
 *           Android 12 sensorservice lo usa para resolver el targetSdkVersion del llamante
 *           -> lanza una RemoteException dentro del provider:
 *             E/JavaBinder: *** Uncaught remote exception!
 *             Caused by: android.os.RemoteException: Couldn't get targetSdkVersion for
 *             package camera.msm8916
 *           (coincide en el tiempo con el 'received signal 13' (SIGPIPE) con el que init
 *           mato al provider). Se usa "android", que SI es un paquete instalado
 *           (framework-res, application con hasCode=false) y por tanto resoluble.
 */
void _ZN7android13SensorManagerC1Ev(void *sensorMgr)
{
    void *string;

    _ZN7android8String16C1EPKc(&string, "android");
    _ZN7android13SensorManagerC1ERKNS_8String16E(sensorMgr, &string);
    _ZN7android8String16D1Ev(&string);
}

/*
 * FUNCTION: android::SensorManager::createEventQueue(void)
 * USE:      INTERPOSE: create an event queue to receive events
 * NOTES:    This function no longer exists in M, instead now one must pass
 *           in a client name as a "string8" and an integer "mode"to it. M
 *           sources list default values for these params as an empty string
 *           and 0. So we'll craft the same call here.
 */
void _ZN7android13SensorManager16createEventQueueEv(void **retVal, void *sensorMgr)
{
    void *string8;
    void *string16;

    _ZN7android7String8C1EPKc(&string8, "nvcamera-legacy");
    _ZN7android8String16C1EPKc(&string16, "camera.tegra");
    /* [mocha] modo NORMAL = 0, NO -1.
     * frameworks/native/services/sensorservice/SensorService.cpp:1305 (lineage-19.1):
     *   if (requestedMode != NORMAL && requestedMode != DATA_INJECTION) return nullptr;
     * (NORMAL = 0, DATA_INJECTION = 1: SensorService.h:282/292)
     * El shim original venia de CM/M con -1, que entonces se aceptaba. En Android 12 -1 hace
     * que el SensorService devuelva nullptr -> libsensor loguea "createEventQueue: connection
     * is NULL" -> NvDeviceOrientation::initialize falla -> NvCameraHal3Core "Sensor Listener
     * Failed to Initialize" -> NvCameraHal3 "open: Camera Open Failed" -> el HAL destruye el
     * dispositivo y ~NvDeviceOrientation::disable() llama a disableSensor() sobre la queue NULL
     * (SIGSEGV, fault addr 0x10, r0=0), matando el provider en bucle. Medido 2026-10-28.
     * El -1 de CM servia para "cualquier modo"; aqui hay que pedir el modo normal. */
    _ZN7android13SensorManager16createEventQueueENS_7String8EiNS_8String16E(retVal, sensorMgr, &string8, 0 /*NORMAL*/, &string16);
    _ZN7android7String8D1Ev(&string8);
    _ZN7android8String16D1Ev(&string16);
}

/*
 * FUNCTION: libEvtLoading()
 * USE:      Handle library loading
 * NOTES:    This is a good time to log the fact that we were loaded and plan to
 *           do our thing.
 */
void libEvtLoading(void)
{
    ALOGV("Sensor manager interposition library loaded");
}

/*
 * FUNCTION: libEvtUnloading()
 * USE:      Handle library unloading
 * NOTES:    This is a good time to free whatever is unfreed and say goodbye
 */
void libEvtUnloading(void)
{
    ALOGV("Sensor manager interposition library unloading. Goodbye...");
    if (_ZN7android9SingletonINS_13SensorManagerEE9sInstanceE) {
        //if an instance stil exists, free it by calling the destructor, just to be throrough
        _ZN7android13SensorManagerD1Ev(_ZN7android9SingletonINS_13SensorManagerEE9sInstanceE);
        _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = NULL;
    }
}
