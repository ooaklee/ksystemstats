/*
    SPDX-FileCopyrightText: 2026 Leon Silcott <lnsilcott@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QByteArray>
#include <QHash>

#include <optional>

class QIODevice;

struct LinuxDrmFdInfo {
    QByteArray driver;
    std::optional<quint64> clientId;
    QHash<QByteArray, quint64> engineTime;
};

struct LinuxMsmClientSample {
    quint64 clientId;
    quint64 gpuEngineTime;
};

LinuxDrmFdInfo parseLinuxDrmFdInfo(QIODevice *device);
std::optional<LinuxMsmClientSample> linuxMsmClientSample(const LinuxDrmFdInfo &fdInfo);
void mergeLinuxMsmClientSample(QHash<quint64, quint64> &clients, const LinuxMsmClientSample &sample);

class LinuxDrmUsageSampler
{
public:
    double sample(const QHash<quint64, quint64> &currentEngineTimes, qint64 elapsedNanoseconds);
    void reset();

private:
    QHash<quint64, quint64> m_previousEngineTimes;
    bool m_hasBaseline = false;
};
