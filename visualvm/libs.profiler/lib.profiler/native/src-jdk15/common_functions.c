/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
/*
 * author Ian Formanek
 *        Tomas Hurka
 *        Misha Dimitiev
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jvmti.h"

#include "common_functions.h"

jvmtiEnv            *_jvmti;
jvmtiEventCallbacks *_jvmti_callbacks;

static jlong _nano_time;
static jvmtiEventCallbacks _jvmti_callbacks_static;

/** A convenience function for the high-resolution timer */
jlong get_nano_time() {
    (*_jvmti)->GetTime(_jvmti, &_nano_time);
    return _nano_time;
}


/** Report the correct usage in case we think the user is trying to launch the VM on its own */
void report_usage() {
    fprintf(stderr, "Profiler Agent: -agentpath:<PATH>/profilerinterface should be called with two parameters:\n");
    fprintf(stderr, "Profiler Agent: path to Profiler agent libraries and port number, separated by comma, for example:\n");
    fprintf(stderr, "Profiler Agent: java -agentpath:/mypath/profilerinterface=/home/me/nb-profiler-server/profiler-ea-libs,5500\n");
}

static void initializeJVMTI(JavaVM *jvm) {
    jvmtiError err;
    jvmtiCapabilities capas;
    jint res;

    /* Obtain the JVMTI environment to be used by this agent */
#ifdef JNI_VERSION_1_6
    (*jvm)->GetEnv(jvm, (void**)&_jvmti, JVMTI_VERSION_1_1);
#else
    (*jvm)->GetEnv(jvm, (void**)&_jvmti, JVMTI_VERSION_1_0);
#endif

    /* Enable runtime class redefinition capability */
    err = (*_jvmti)->GetCapabilities(_jvmti, &capas);
    assert(err == JVMTI_ERROR_NONE);
    capas.can_redefine_classes = 1;
#ifdef JNI_VERSION_1_6
    capas.can_retransform_classes = 1;
#endif
    capas.can_generate_garbage_collection_events = 1;
    capas.can_generate_native_method_bind_events = 1;
    capas.can_generate_monitor_events = 1;
    capas.can_get_current_thread_cpu_time = 1;
    capas.can_generate_vm_object_alloc_events = 1;
    capas.can_get_monitor_info = 1;
    err = (*_jvmti)->AddCapabilities(_jvmti, &capas);
    if (err != JVMTI_ERROR_NONE) {
        fprintf(stderr, "Profiler Agent Error: Failed to obtain JVMTI capabilities, error code: %d\n", err);
    }

    /* Zero out the callbacks data structure for future use*/
    _jvmti_callbacks = &_jvmti_callbacks_static;
    memset(_jvmti_callbacks, 0, sizeof(jvmtiEventCallbacks));

    /* Enable class load hook event, that captures class file bytes for classes loaded by non-system loaders */
    _jvmti_callbacks->ClassFileLoadHook = class_file_load_hook;
    _jvmti_callbacks->NativeMethodBind = native_method_bind_hook;
    _jvmti_callbacks->MonitorContendedEnter = monitor_contended_enter_hook;
    _jvmti_callbacks->MonitorContendedEntered = monitor_contended_entered_hook;
    _jvmti_callbacks->VMObjectAlloc = vm_object_alloc;
    res = (*_jvmti)->SetEventCallbacks(_jvmti, _jvmti_callbacks, sizeof(*_jvmti_callbacks));
    assert (res == JVMTI_ERROR_NONE);

    res = (*_jvmti)->SetEventNotificationMode(_jvmti, JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, NULL);
    assert(res == JVMTI_ERROR_NONE);

    res = (*_jvmti)->SetEventNotificationMode(_jvmti, JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, NULL);
    assert(res == JVMTI_ERROR_NONE);

    res = (*_jvmti)->SetEventNotificationMode(_jvmti, JVMTI_ENABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTER, NULL);
    assert(res == JVMTI_ERROR_NONE);

    res = (*_jvmti)->SetEventNotificationMode(_jvmti, JVMTI_ENABLE, JVMTI_EVENT_MONITOR_CONTENDED_ENTERED, NULL);
    assert(res == JVMTI_ERROR_NONE);
}

/* The VM calls this function when the native library is loaded (for example, through System.loadLibrary). */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved) {
    if (_jvmti == NULL) {
        fprintf(stdout, "Profiler Agent: JNI OnLoad Initializing...\n");

        initializeJVMTI(jvm);
    
        fprintf(stdout, "Profiler Agent: JNI OnLoad Initialized successfully\n");
    }
    return JNI_VERSION_1_2;
}

/** This function is called automatically upon agent startup */
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options, void *reserved) {
    fprintf(stdout, "Profiler Agent: Initializing...\n");

    initializeJVMTI(jvm);

    if (options != NULL) {
      fprintf (stdout, "Profiler Agent: Options: >%s<\n", options);
    } else {
      fprintf (stdout, "Profiler Agent: No options\n");
    }    

    /* If it looks like the VM was started not from the tool, but on its own, e.g. like
    java -agentpath:/blahblah/profilerinterface=/foobar/profiler-ea-libs,5500
    do some sanity checks for options and then eable the VM init event, so that we can start
    our Java agent when the VM is initialized */
    if (options != NULL && strlen(options) > 0) { /* The spec says no options means options == "", but in reality it's NULL */
        if (strpbrk(options, ",") == NULL) {
            report_usage();
            return -1;
        } else {  /* We believe the options are correct */
            parse_options_and_extract_params(options);
            _jvmti_callbacks->VMInit = vm_init_hook;
            (*_jvmti)->SetEventCallbacks(_jvmti, _jvmti_callbacks, sizeof(*_jvmti_callbacks));
            (*_jvmti)->SetEventNotificationMode(_jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, NULL);
        }
    } // in case of calibration, the arguments are just empty, this is OK

    fprintf(stdout, "Profiler Agent: Initialized successfully\n");
    return 0;
}


