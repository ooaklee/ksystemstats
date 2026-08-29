/*
    SPDX-FileCopyrightText: 2026 Leon Silcott <lnsilcott@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "LinuxDrmFdInfo.h"

#include <QIODevice>

#include <algorithm>

namespace
{
std::optional<quint64> parseUnsigned(const QByteArray &input, const QByteArray &unit = {})
{
    QByteArray number = input.trimmed();
    if (!unit.isEmpty()) {
        if (!number.endsWith(unit)) {
            return std::nullopt;
        }
        number.chop(unit.size());
        number = number.trimmed();
    }

    if (number.isEmpty() || !std::all_of(number.cbegin(), number.cend(), [](char character) {
            return character >= '0' && character <= '9';
        })) {
        return std::nullopt;
    }

    bool ok = false;
    const quint64 value = number.toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}
}

LinuxDrmFdInfo parseLinuxDrmFdInfo(QIODevice *device)
{
    LinuxDrmFdInfo result;
    if (!device || !device->isReadable()) {
        return result;
    }

    while (true) {
        const QByteArray line = device->readLine();
        if (line.isNull()) {
            break;
        }
        const qsizetype separator = line.indexOf(':');
        if (separator <= 0) {
            continue;
        }

        const QByteArray key = line.left(separator).trimmed();
        const QByteArray value = line.mid(separator + 1).trimmed();

        if (key == "drm-driver") {
            result.driver = value;
        } else if (key == "drm-client-id") {
            result.clientId = parseUnsigned(value);
        } else if (key.startsWith("drm-engine-")) {
            const QByteArray engine = key.mid(sizeof("drm-engine-") - 1);
            if (!engine.isEmpty()) {
                if (const auto parsed = parseUnsigned(value, "ns")) {
                    result.engineTime.insert(engine, *parsed);
                }
            }
        }
    }

    return result;
}

std::optional<LinuxMsmClientSample> linuxMsmClientSample(const LinuxDrmFdInfo &fdInfo)
{
    const auto gpuEngine = fdInfo.engineTime.constFind("gpu");
    if (fdInfo.driver != "msm" || !fdInfo.clientId || gpuEngine == fdInfo.engineTime.cend()) {
        return std::nullopt;
    }

    return LinuxMsmClientSample{.clientId = *fdInfo.clientId, .gpuEngineTime = gpuEngine.value()};
}

void mergeLinuxMsmClientSample(QHash<quint64, quint64> &clients, const LinuxMsmClientSample &sample)
{
    const auto existing = clients.constFind(sample.clientId);
    if (existing == clients.cend() || sample.gpuEngineTime > existing.value()) {
        clients.insert(sample.clientId, sample.gpuEngineTime);
    }
}

double LinuxDrmUsageSampler::sample(const QHash<quint64, quint64> &currentEngineTimes, qint64 elapsedNanoseconds)
{
    if (!m_hasBaseline) {
        m_previousEngineTimes = currentEngineTimes;
        m_hasBaseline = true;
        return 0.0;
    }

    if (elapsedNanoseconds <= 0) {
        return 0.0;
    }

    auto nextEngineTimes = currentEngineTimes;
    long double busyNanoseconds = 0.0L;

    for (auto it = currentEngineTimes.cbegin(); it != currentEngineTimes.cend(); ++it) {
        const auto previous = m_previousEngineTimes.constFind(it.key());
        if (previous == m_previousEngineTimes.cend()) {
            continue;
        }

        if (it.value() < previous.value()) {
            nextEngineTimes.insert(it.key(), previous.value());
            continue;
        }

        busyNanoseconds += static_cast<long double>(it.value() - previous.value());
    }

    m_previousEngineTimes = nextEngineTimes;
    return std::clamp(static_cast<double>((busyNanoseconds / static_cast<long double>(elapsedNanoseconds)) * 100.0L), 0.0, 100.0);
}

void LinuxDrmUsageSampler::reset()
{
    m_previousEngineTimes.clear();
    m_hasBaseline = false;
}
