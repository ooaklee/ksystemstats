/*
    SPDX-FileCopyrightText: 2026 Leon Silcott <lnsilcott@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "LinuxMsmGpu.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <libudev.h>
#include <sys/stat.h>

#include <systemstats/SensorProperty.h>

namespace
{
constexpr double hertzPerMegahertz = 1'000'000.0;

QString canonicalDevicePath(const QString &drmPath)
{
    return QFileInfo(drmPath + QStringLiteral("/device")).canonicalFilePath();
}

bool addDeviceNumber(udev_device *device, QSet<quint64> &deviceNumbers)
{
    const dev_t deviceNumber = udev_device_get_devnum(device);
    if (deviceNumber == 0) {
        return false;
    }

    deviceNumbers.insert(static_cast<quint64>(deviceNumber));
    return true;
}

bool isAdrenoDevfreq(const QString &path)
{
    QFile uevent(path + QStringLiteral("/device/uevent"));
    if (uevent.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto entries = uevent.readAll().split('\n');
        if (entries.contains(QByteArrayLiteral("DRIVER=adreno"))) {
            return true;
        }
    }

    QFile compatible(path + QStringLiteral("/device/of_node/compatible"));
    if (!compatible.open(QIODevice::ReadOnly)) {
        return false;
    }

    for (const QByteArray &entry : compatible.readAll().split('\0')) {
        if (entry == QByteArrayLiteral("qcom,adreno") || entry.startsWith(QByteArrayLiteral("qcom,adreno-"))) {
            return true;
        }
    }
    return false;
}
}

LinuxMsmGpu::LinuxMsmGpu(const QString &id, const QString &name, udev_device *device)
    : GpuDevice(id, name)
{
    discoverDeviceNumbers(device);
    m_devfreqPath = findDevfreqPath(device);
}

void LinuxMsmGpu::initialize()
{
    GpuDevice::initialize();

    m_usageProperty->setValue(0.0);

    bool ok = false;
    const double minimumMhz = readFrequencyMhz(QStringLiteral("min_freq"), &ok);
    if (ok) {
        m_coreFrequencyProperty->setMin(minimumMhz);
    }

    const double maximumMhz = readFrequencyMhz(QStringLiteral("max_freq"), &ok);
    if (ok) {
        m_coreFrequencyProperty->setMax(maximumMhz);
    }

    const double currentMhz = readFrequencyMhz(QStringLiteral("cur_freq"), &ok);
    if (ok) {
        m_coreFrequencyProperty->setValue(currentMhz);
    }

    connect(m_usageProperty, &KSysGuard::SensorProperty::subscribedChanged, this, [this](bool subscribed) {
        m_usageSubscribed = subscribed;
        m_usageSampler.reset();
        m_usageTimer.invalidate();
        if (subscribed) {
            m_usageTimer.start();
        }
    });
    connect(m_coreFrequencyProperty, &KSysGuard::SensorProperty::subscribedChanged, this, [this](bool subscribed) {
        m_frequencySubscribed = subscribed;
    });
}

void LinuxMsmGpu::update()
{
    if (m_frequencySubscribed) {
        bool ok = false;
        const double currentMhz = readFrequencyMhz(QStringLiteral("cur_freq"), &ok);
        if (ok) {
            m_coreFrequencyProperty->setValue(currentMhz);
        }
    }

    if (!m_usageSubscribed || !m_usageTimer.isValid()) {
        return;
    }

    const auto engineTimes = readClientEngineTimes();
    const qint64 elapsedNanoseconds = m_usageTimer.nsecsElapsed();
    m_usageTimer.restart();
    m_usageProperty->setValue(m_usageSampler.sample(engineTimes, elapsedNanoseconds));
}

void LinuxMsmGpu::discoverDeviceNumbers(udev_device *device)
{
    addDeviceNumber(device, m_deviceNumbers);

    const char *devicePath = udev_device_get_syspath(device);
    udev *udevContext = udev_device_get_udev(device);
    if (!devicePath || !udevContext) {
        return;
    }

    const QString drmDevicePath = canonicalDevicePath(QString::fromLocal8Bit(devicePath));
    if (drmDevicePath.isEmpty()) {
        return;
    }

    static const QRegularExpression drmNodePattern(QStringLiteral("^(?:card[0-9]+|renderD[0-9]+)$"));
    udev_enumerate *enumerate = udev_enumerate_new(udevContext);
    if (!enumerate) {
        return;
    }
    udev_enumerate_add_match_property(enumerate, "DEVTYPE", "drm_minor");
    udev_enumerate_add_match_subsystem(enumerate, "drm");
    udev_enumerate_scan_devices(enumerate);

    for (auto entry = udev_enumerate_get_list_entry(enumerate); entry; entry = udev_list_entry_get_next(entry)) {
        const char *path = udev_list_entry_get_name(entry);
        udev_device *candidate = udev_device_new_from_syspath(udevContext, path);
        if (!candidate) {
            continue;
        }

        const char *systemName = udev_device_get_sysname(candidate);
        const char *candidatePath = udev_device_get_syspath(candidate);
        if (systemName && candidatePath && drmNodePattern.match(QString::fromLocal8Bit(systemName)).hasMatch()
            && canonicalDevicePath(QString::fromLocal8Bit(candidatePath)) == drmDevicePath) {
            addDeviceNumber(candidate, m_deviceNumbers);
        }
        udev_device_unref(candidate);
    }
    udev_enumerate_unref(enumerate);
}

QString LinuxMsmGpu::findDevfreqPath(udev_device *device) const
{
    const char *devicePath = udev_device_get_syspath(device);
    if (!devicePath) {
        return {};
    }

    const QString drmDevicePath = canonicalDevicePath(QString::fromLocal8Bit(devicePath));
    const QString drmModulePath = QFileInfo(drmDevicePath + QStringLiteral("/driver/module")).canonicalFilePath();
    QStringList candidates;
    QStringList moduleCandidates;

    const QDir devfreqClass(QStringLiteral("/sys/class/devfreq"));
    const auto entries = devfreqClass.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString path = devfreqClass.filePath(entry);
        if (!isAdrenoDevfreq(path)) {
            continue;
        }

        const QString candidateDevicePath = QFileInfo(path + QStringLiteral("/device")).canonicalFilePath();
        if (!drmDevicePath.isEmpty() && candidateDevicePath == drmDevicePath) {
            return path;
        }

        candidates.append(path);
        const QString candidateModulePath = QFileInfo(path + QStringLiteral("/device/driver/module")).canonicalFilePath();
        if (!drmModulePath.isEmpty() && candidateModulePath == drmModulePath) {
            moduleCandidates.append(path);
        }
    }

    if (moduleCandidates.size() == 1) {
        return moduleCandidates.constFirst();
    }
    if (candidates.size() == 1) {
        return candidates.constFirst();
    }
    return {};
}

QHash<quint64, quint64> LinuxMsmGpu::readClientEngineTimes() const
{
    QHash<quint64, quint64> engineTimes;
    const QDir proc(QStringLiteral("/proc"));
    const auto processDirectories = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);

    // fdinfo visibility is permission-limited. In an unprivileged user
    // session this therefore represents the visible (normally same-UID)
    // clients, rather than a privileged system-wide accounting source.
    for (const QString &processName : processDirectories) {
        bool isPid = false;
        processName.toUInt(&isPid);
        if (!isPid) {
            continue;
        }

        const QString processRoot = proc.filePath(processName);
        const QDir fdInfoDirectory(processRoot + QStringLiteral("/fdinfo"));
        const auto descriptors = fdInfoDirectory.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDir::NoSort);

        for (const QString &descriptor : descriptors) {
            const QString descriptorPath = processRoot + QStringLiteral("/fd/") + descriptor;
            struct stat descriptorStat;
            if (stat(QFile::encodeName(descriptorPath).constData(), &descriptorStat) != 0 || !S_ISCHR(descriptorStat.st_mode)
                || !m_deviceNumbers.contains(static_cast<quint64>(descriptorStat.st_rdev))) {
                continue;
            }

            QFile fdInfo(fdInfoDirectory.filePath(descriptor));
            if (!fdInfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
                continue;
            }

            struct stat verifiedDescriptorStat;
            if (stat(QFile::encodeName(descriptorPath).constData(), &verifiedDescriptorStat) != 0 || descriptorStat.st_rdev != verifiedDescriptorStat.st_rdev) {
                continue;
            }

            if (const auto sample = linuxMsmClientSample(parseLinuxDrmFdInfo(&fdInfo))) {
                mergeLinuxMsmClientSample(engineTimes, *sample);
            }
        }
    }

    return engineTimes;
}

double LinuxMsmGpu::readFrequencyMhz(const QString &fileName, bool *ok) const
{
    if (ok) {
        *ok = false;
    }

    if (m_devfreqPath.isEmpty()) {
        return 0.0;
    }

    QFile frequencyFile(m_devfreqPath + QLatin1Char('/') + fileName);
    if (!frequencyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0.0;
    }

    bool parsed = false;
    const qulonglong hertz = frequencyFile.readAll().trimmed().toULongLong(&parsed);
    if (ok) {
        *ok = parsed;
    }
    return parsed ? static_cast<double>(hertz) / hertzPerMegahertz : 0.0;
}

#include "moc_LinuxMsmGpu.cpp"
