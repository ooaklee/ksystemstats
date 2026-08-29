/*
 * SPDX-FileCopyrightText: 2020 Arjen Hiemstra <ahiemstra@heimr.nl>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "LinuxBackend.h"

#include <KLocalizedString>

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <libudev.h>

#include <optional>

#include <processcore/gpu_utils.h>

#include "LinuxAmdGpu.h"
#include "LinuxDrmFdInfo.h"
#include "LinuxIntelGpu.h"
#include "LinuxMsmGpu.h"
#include "LinuxNvidiaGpu.h"
#ifdef HAVE_XE_DRM_H
#include "LinuxXeGpu.h"
#endif
#include "debug.h"

// Vendor ID strings, as used in sysfs
static const char *amdVendor = "0x1002";
static const char *intelVendor = "0x8086";
static const char *nvidiaVendor = "0x10de";
// PCI Device class strings
static const char *VGAController = "0x030000";
static const char *threeDController = "0x030200";
static const char *displayController = "0x038000";

static bool isXeDriver(udev_device *pciDevice)
{
    const char *driver = udev_device_get_driver(pciDevice);
    return driver && strcmp(driver, "xe") == 0;
}

static std::optional<LinuxDrmFdInfo> readDrmFdInfo(udev_device *drmDevice)
{
    const char *deviceNode = udev_device_get_devnode(drmDevice);
    if (!deviceNode) {
        return std::nullopt;
    }

    QFile drmNode(QString::fromLocal8Bit(deviceNode));
    if (!drmNode.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }

    QFile fdInfo(QStringLiteral("/proc/self/fdinfo/%1").arg(drmNode.handle()));
    if (!fdInfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }

    return parseLinuxDrmFdInfo(&fdInfo);
}

static bool isMsmGpu(udev_device *drmDevice)
{
    const char *devicePath = udev_device_get_syspath(drmDevice);
    udev *udevContext = udev_device_get_udev(drmDevice);
    if (!devicePath || !udevContext) {
        return false;
    }

    const QString physicalDevicePath = QFileInfo(QString::fromLocal8Bit(devicePath) + QStringLiteral("/device")).canonicalFilePath();
    if (physicalDevicePath.isEmpty()) {
        return false;
    }

    // Render nodes are normally accessible without display-master privileges,
    // so prefer a render node belonging to the same physical DRM device.
    QStringList renderDevicePaths;
    udev_enumerate *enumerate = udev_enumerate_new(udevContext);
    if (enumerate) {
        udev_enumerate_add_match_property(enumerate, "DEVTYPE", "drm_minor");
        udev_enumerate_add_match_subsystem(enumerate, "drm");
        udev_enumerate_scan_devices(enumerate);

        static const QRegularExpression renderNodePattern(QStringLiteral("^renderD[0-9]+$"));
        for (auto entry = udev_enumerate_get_list_entry(enumerate); entry; entry = udev_list_entry_get_next(entry)) {
            const char *path = udev_list_entry_get_name(entry);
            udev_device *candidate = udev_device_new_from_syspath(udevContext, path);
            if (!candidate) {
                continue;
            }

            const char *systemName = udev_device_get_sysname(candidate);
            const char *candidatePath = udev_device_get_syspath(candidate);
            if (systemName && candidatePath && renderNodePattern.match(QString::fromLocal8Bit(systemName)).hasMatch()
                && QFileInfo(QString::fromLocal8Bit(candidatePath) + QStringLiteral("/device")).canonicalFilePath() == physicalDevicePath) {
                renderDevicePaths.append(QString::fromLocal8Bit(candidatePath));
            }
            udev_device_unref(candidate);
        }
        udev_enumerate_unref(enumerate);
    }

    for (const QString &path : std::as_const(renderDevicePaths)) {
        udev_device *renderDevice = udev_device_new_from_syspath(udevContext, QFile::encodeName(path).constData());
        if (!renderDevice) {
            continue;
        }
        const auto fdInfo = readDrmFdInfo(renderDevice);
        udev_device_unref(renderDevice);
        if (fdInfo && fdInfo->driver == "msm") {
            return true;
        }
    }

    const auto fdInfo = readDrmFdInfo(drmDevice);
    return fdInfo && linuxMsmClientSample(*fdInfo).has_value();
}

LinuxBackend::LinuxBackend(QObject *parent)
    : GpuBackend(parent)
{
}

void LinuxBackend::start()
{
    if (!m_udev) {
        m_udev = udev_new();
    }

    auto enumerate = udev_enumerate_new(m_udev);

    udev_enumerate_add_match_subsystem(enumerate, "pci");
    udev_enumerate_add_match_sysattr(enumerate, "class", VGAController);
    udev_enumerate_add_match_sysattr(enumerate, "class", threeDController);
    udev_enumerate_add_match_sysattr(enumerate, "class", displayController);
    udev_enumerate_scan_devices(enumerate);

    auto devices = udev_enumerate_get_list_entry(enumerate);
    int gpuNumber = 0;
    for (auto entry = devices; entry; entry = udev_list_entry_get_next(entry)) {
        auto path = udev_list_entry_get_name(entry);
        auto pciDevice = udev_device_new_from_syspath(m_udev, path);

        auto vendor = QByteArray(udev_device_get_sysattr_value(pciDevice, "vendor"));
        auto gpuId = QStringLiteral("gpu%1").arg(gpuNumber);

        QString gpuName = KSysGuard::gpuName(pciDevice, gpuNumber);

        GpuDevice *gpu = nullptr;
        if (isXeDriver(pciDevice)) {
#ifdef HAVE_XE_DRM_H
            gpu = new LinuxXeGpu{gpuId, gpuName, pciDevice};
#else
            qCWarning(KSYSTEMSTATS_GPU) << "Found Xe GPU but ksystemstats compiled without Xe support";
            udev_device_unref(pciDevice);
            continue;
#endif
        } else if (vendor == amdVendor) {
            gpu = new LinuxAmdGpu{gpuId, gpuName, pciDevice};
        } else if (vendor == nvidiaVendor) {
            gpu = new LinuxNvidiaGpu{gpuId, gpuName, pciDevice};
        } else if (vendor == intelVendor) {
            gpu = new LinuxIntelGpu{gpuId, gpuName, pciDevice};
        }

        if (!gpu) {
            qCDebug(KSYSTEMSTATS_GPU) << "Found unsupported GPU:" << path;
            udev_device_unref(pciDevice);
            continue;
        }

        gpu->initialize();
        m_devices.append(gpu);
        Q_EMIT deviceAdded(gpu);
        gpuNumber++;

        udev_device_unref(pciDevice);
    }

    udev_enumerate_unref(enumerate);

    // PCI enumeration above intentionally includes headless PCI GPUs. Platform
    // GPUs have no PCI vendor ID, so discover DRM primary nodes separately and
    // classify them using the mandatory drm-driver fdinfo field.
    enumerate = udev_enumerate_new(m_udev);
    udev_enumerate_add_match_property(enumerate, "DEVTYPE", "drm_minor");
    udev_enumerate_add_match_subsystem(enumerate, "drm");
    udev_enumerate_scan_devices(enumerate);

    static const QRegularExpression primaryNodePattern(QStringLiteral("^card[0-9]+$"));
    devices = udev_enumerate_get_list_entry(enumerate);
    for (auto entry = devices; entry; entry = udev_list_entry_get_next(entry)) {
        const char *path = udev_list_entry_get_name(entry);
        auto drmDevice = udev_device_new_from_syspath(m_udev, path);
        if (!drmDevice) {
            continue;
        }
        const char *systemName = udev_device_get_sysname(drmDevice);

        if (!systemName || !primaryNodePattern.match(QString::fromLocal8Bit(systemName)).hasMatch()
            || udev_device_get_parent_with_subsystem_devtype(drmDevice, "pci", nullptr) || !isMsmGpu(drmDevice)) {
            udev_device_unref(drmDevice);
            continue;
        }

        const auto gpuId = QStringLiteral("gpu%1").arg(gpuNumber);
        const QString gpuName = i18nc("@title", "Qualcomm Adreno GPU");
        auto gpu = new LinuxMsmGpu{gpuId, gpuName, drmDevice};
        gpu->initialize();
        m_devices.append(gpu);
        Q_EMIT deviceAdded(gpu);
        gpuNumber++;

        udev_device_unref(drmDevice);
    }

    udev_enumerate_unref(enumerate);
}

void LinuxBackend::stop()
{
    qDeleteAll(m_devices);
    udev_unref(m_udev);
}

void LinuxBackend::update()
{
    for (auto device : std::as_const(m_devices)) {
        device->update();
    }
}

int LinuxBackend::deviceCount()
{
    return m_devices.count();
}

#include "moc_LinuxBackend.cpp"
