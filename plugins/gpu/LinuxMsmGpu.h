/*
    SPDX-FileCopyrightText: 2026 Leon Silcott <lnsilcott@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "GpuDevice.h"
#include "LinuxDrmFdInfo.h"

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <optional>

struct udev_device;

class LinuxMsmGpu : public GpuDevice
{
    Q_OBJECT

public:
    LinuxMsmGpu(const QString &id, const QString &name, udev_device *device);

    void initialize() override;
    void update() override;

private:
    void discoverDeviceNumbers(udev_device *device);
    QString findDevfreqPath(udev_device *device) const;
    std::optional<LinuxMsmClientSample> readClientEngineTime(const QString &processRoot, const QString &descriptor, bool &valid) const;
    QHash<quint64, quint64> readClientEngineTimes(bool discover);
    double readFrequencyMhz(const QString &fileName, bool *ok = nullptr) const;

    QSet<quint64> m_deviceNumbers;
    QString m_devfreqPath;
    QHash<QString, QSet<QString>> m_clientDescriptors;
    QElapsedTimer m_clientDiscoveryTimer;
    LinuxDrmUsageSampler m_usageSampler;
    QElapsedTimer m_usageTimer;
    bool m_usageSubscribed = false;
    bool m_frequencySubscribed = false;
};
