/*
 * Copyright (c) 2021-2024 Estonian Information System Authority
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include "controllerchildthread.hpp"

#include "utils/utils.hpp"

class CardEventMonitorThread : public ControllerChildThread
{
    Q_OBJECT

public:
    using eid_ptr = electronic_id::ElectronicID::ptr;

    CardEventMonitorThread(QObject* parent, CommandType commandType, eid_ptr eid) :
        ControllerChildThread(commandType, parent), eid(std::move(eid)),
        monitor(makeMonitor(this->eid))
    {
    }

    void cancelWait()
    {
        requestInterruption();
        monitor.cancel();
    }

    void run() override
    {
        qDebug() << "Starting" << metaObject()->className() << uintptr_t(this) << "for command"
                 << commandType();

        if (monitor.wait() && !isInterruptionRequested()) {
            qDebug() << metaObject()->className() << "card change detected";
            emit cardEvent();
        }
    }

signals:
    void cardEvent();

private:
    void doRun() override
    {
        // Unused as run() has been overridden.
    }

    static pcsc_cpp::CardEventMonitor makeMonitor(const eid_ptr& eid)
    {
        return pcsc_cpp::CardEventMonitor(eid ? &eid->smartcard() : nullptr);
    }

    eid_ptr eid;
    pcsc_cpp::CardEventMonitor monitor;
};
