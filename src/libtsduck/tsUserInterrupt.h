//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2017, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  User interrupt handling (Ctrl+C).
//
//----------------------------------------------------------------------------

#pragma once
#include "tsInterruptHandler.h"
#include "tsThread.h"

namespace ts {

    // An instance of this class handles the Ctrl+C user interrupt.
    // There must be at most one active instance at a time.
    //
    // Can be used in two ways:
    // - Interrupt notification through one InterruptHandler
    // - Interrupt polling through isInterrupted()/resetInterrupted().

    class TSDUCKDLL UserInterrupt
#if defined(__unix)
        : private Thread
#endif
    {
    public:
        // Constructor.
        // Handler may be null.
        // If one_shot is true, the interrupt will be handled only once,
        // the second time the process will be terminated.
        UserInterrupt(InterruptHandler* handler, bool one_shot, bool auto_activate);

        // Destructor, auto-deactivate
        ~UserInterrupt();

        // Check if active
        bool isActive() const {return _active;}

        // Check if interrupt was triggered.
        bool isInterrupted() const {return _interrupted;}

        // Reset interrupt state
        void resetInterrupted() {_interrupted = false;}

        // Activate/deactivate
        void activate();
        void deactivate();

    private:
#if defined(__windows)

        static ::BOOL WINAPI sysHandler(__in ::DWORD dwCtrlType);

#elif defined(__unix)

        static void sysHandler(int sig);
        virtual void main(); // ts::Thread implementation
        
        volatile bool           _terminate;
        volatile ::sig_atomic_t _got_sigint;
#if defined(__mac)
        std::string             _sem_name;
        ::sem_t*                _sem_address;
#else
        ::sem_t                 _sem_instance;
#endif
        
#endif

        InterruptHandler* _handler;
        bool              _one_shot;
        bool              _active;
        volatile bool     _interrupted;

        // There is only one active instance at a time
        static UserInterrupt* volatile _active_instance;

        // Inaccessible operations
        UserInterrupt(const UserInterrupt&) = delete;
        UserInterrupt& operator=(const UserInterrupt&) = delete;
    };
}