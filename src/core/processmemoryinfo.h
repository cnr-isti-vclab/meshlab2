#pragma once

#include <QtGlobal>

// OS-reported process metrics. These values deliberately remain separate because
// their accounting semantics differ between platforms and from MeshLab's owned
// resource counters.
struct ProcessMemoryInfo {
    qint64 physicalFootprintBytes = -1;
    qint64 residentBytes = -1;
    qint64 privateBytes = -1;

    qint64 preferredBytes() const
    {
        if (physicalFootprintBytes >= 0)
            return physicalFootprintBytes;
        if (privateBytes >= 0)
            return privateBytes;
        return residentBytes;
    }
};

ProcessMemoryInfo queryCurrentProcessMemoryInfo();
