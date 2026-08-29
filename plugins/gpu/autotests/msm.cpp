/*
    SPDX-FileCopyrightText: 2026 Leon Silcott <lnsilcott@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include <QBuffer>
#include <QTest>

#include "../LinuxDrmFdInfo.h"

#include <algorithm>
#include <cstring>
#include <utility>

class ProcLikeDevice : public QIODevice
{
public:
    explicit ProcLikeDevice(QByteArray data)
        : m_data(std::move(data))
    {
        open(QIODevice::ReadOnly);
    }

    bool atEnd() const override
    {
        return true;
    }

protected:
    qint64 readData(char *data, qint64 maximumSize) override
    {
        if (m_offset >= m_data.size()) {
            return -1;
        }

        const qint64 count = std::min(maximumSize, static_cast<qint64>(m_data.size() - m_offset));
        std::memcpy(data, m_data.constData() + m_offset, static_cast<size_t>(count));
        m_offset += count;
        return count;
    }

    qint64 writeData(const char *, qint64) override
    {
        return -1;
    }

private:
    QByteArray m_data;
    qint64 m_offset = 0;
};

class MsmGpuTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFdInfoParsing();
    void testProcLikeFdInfoParsing();
    void testMalformedNumbers();
    void testInvalidFdInfo_data();
    void testInvalidFdInfo();
    void testMsmClientSample();
    void testDuplicateClientMerging();
    void testUsageSampling();
    void testClientLifecycle();
    void testCounterRegression();
    void testElapsedTimeAndReset();
};

void MsmGpuTest::testFdInfoParsing()
{
    QByteArray data = QByteArrayLiteral(
        "pos:\t0\n"
        "drm-driver:\tmsm\n"
        "drm-client-id:\t42\n"
        "drm-engine-gpu:\t123456 ns\n"
        "drm-engine-capacity-gpu:\t2\n"
        "drm-cycles-gpu:\t789\n");
    QBuffer buffer(&data);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const LinuxDrmFdInfo fdInfo = parseLinuxDrmFdInfo(&buffer);
    QCOMPARE(fdInfo.driver, QByteArrayLiteral("msm"));
    QVERIFY(fdInfo.clientId.has_value());
    QCOMPARE(*fdInfo.clientId, 42);
    QCOMPARE(fdInfo.engineTime.value(QByteArrayLiteral("gpu")), 123456);
    QVERIFY(!fdInfo.engineTime.contains(QByteArrayLiteral("capacity-gpu")));

    const auto sample = linuxMsmClientSample(fdInfo);
    QVERIFY(sample.has_value());
    QCOMPARE(sample->clientId, 42);
    QCOMPARE(sample->gpuEngineTime, 123456);

    QVERIFY(!linuxMsmClientSample(parseLinuxDrmFdInfo(nullptr)).has_value());
    QBuffer unopenedBuffer;
    QVERIFY(!linuxMsmClientSample(parseLinuxDrmFdInfo(&unopenedBuffer)).has_value());
}

void MsmGpuTest::testProcLikeFdInfoParsing()
{
    ProcLikeDevice device(
        QByteArrayLiteral("drm-driver:\tmsm\n"
                          "drm-client-id:\t7\n"
                          "drm-engine-gpu:\t0 ns\n"));
    QVERIFY(device.atEnd());

    const auto sample = linuxMsmClientSample(parseLinuxDrmFdInfo(&device));
    QVERIFY(sample.has_value());
    QCOMPARE(sample->clientId, 7);
    QCOMPARE(sample->gpuEngineTime, 0);
}

void MsmGpuTest::testMalformedNumbers()
{
    QByteArray data = QByteArrayLiteral(
        "drm-driver: msm\n"
        "drm-client-id: -1\n"
        "drm-engine-gpu: -1 ns\n"
        "drm-engine-copy: 12 us\n"
        "drm-engine-video: invalid ns\n"
        "drm-engine-: 12 ns\n");
    QBuffer buffer(&data);
    QVERIFY(buffer.open(QIODevice::ReadOnly));

    const LinuxDrmFdInfo fdInfo = parseLinuxDrmFdInfo(&buffer);
    QVERIFY(!fdInfo.clientId.has_value());
    QVERIFY(fdInfo.engineTime.isEmpty());
}

void MsmGpuTest::testInvalidFdInfo_data()
{
    QTest::addColumn<QByteArray>("data");

    QTest::newRow("wrong-driver") << QByteArrayLiteral("drm-driver: amdgpu\ndrm-client-id: 1\ndrm-engine-gpu: 20 ns\n");
    QTest::newRow("missing-client") << QByteArrayLiteral("drm-driver: msm\ndrm-engine-gpu: 20 ns\n");
    QTest::newRow("missing-engine") << QByteArrayLiteral("drm-driver: msm\ndrm-client-id: 1\n");
    QTest::newRow("invalid-client") << QByteArrayLiteral("drm-driver: msm\ndrm-client-id: invalid\ndrm-engine-gpu: 20 ns\n");
    QTest::newRow("signed-client") << QByteArrayLiteral("drm-driver: msm\ndrm-client-id: +1\ndrm-engine-gpu: 20 ns\n");
    QTest::newRow("overflowing-client") << QByteArrayLiteral("drm-driver: msm\ndrm-client-id: 18446744073709551616\ndrm-engine-gpu: 20 ns\n");
    QTest::newRow("invalid-unit") << QByteArrayLiteral("drm-driver: msm\ndrm-client-id: 1\ndrm-engine-gpu: 20 us\n");
}

void MsmGpuTest::testInvalidFdInfo()
{
    QFETCH(QByteArray, data);

    QBuffer buffer(&data);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QVERIFY(!linuxMsmClientSample(parseLinuxDrmFdInfo(&buffer)).has_value());
}

void MsmGpuTest::testMsmClientSample()
{
    LinuxDrmFdInfo fdInfo;
    fdInfo.driver = QByteArrayLiteral("msm");
    fdInfo.clientId = 0;
    fdInfo.engineTime.insert(QByteArrayLiteral("gpu"), 0);

    const auto sample = linuxMsmClientSample(fdInfo);
    QVERIFY(sample.has_value());
    QCOMPARE(sample->clientId, 0);
    QCOMPARE(sample->gpuEngineTime, 0);

    fdInfo.driver = QByteArrayLiteral("MSM");
    QVERIFY(!linuxMsmClientSample(fdInfo).has_value());
}

void MsmGpuTest::testDuplicateClientMerging()
{
    QHash<quint64, quint64> clients;
    mergeLinuxMsmClientSample(clients, {.clientId = 7, .gpuEngineTime = 100});
    mergeLinuxMsmClientSample(clients, {.clientId = 7, .gpuEngineTime = 80});
    mergeLinuxMsmClientSample(clients, {.clientId = 7, .gpuEngineTime = 120});
    mergeLinuxMsmClientSample(clients, {.clientId = 8, .gpuEngineTime = 0});

    QCOMPARE(clients.size(), 2);
    QCOMPARE(clients.value(7), 120);
    QCOMPARE(clients.value(8), 0);
}

void MsmGpuTest::testUsageSampling()
{
    LinuxDrmUsageSampler sampler;

    QCOMPARE(sampler.sample({{1, 100}}, 500), 0.0);
    QCOMPARE(sampler.sample({{1, 350}}, 500), 50.0);
    QCOMPARE(sampler.sample({{1, 1350}}, 500), 100.0);
}

void MsmGpuTest::testClientLifecycle()
{
    LinuxDrmUsageSampler sampler;

    QCOMPARE(sampler.sample({{1, 100}}, 100), 0.0);
    QCOMPARE(sampler.sample({{1, 150}, {2, 500}}, 100), 50.0);
    QCOMPARE(sampler.sample({{2, 550}}, 100), 50.0);
    QCOMPARE(sampler.sample({{1, 1000}, {2, 600}}, 100), 50.0);
    QCOMPARE(sampler.sample({{1, 1025}, {2, 625}}, 100), 50.0);
}

void MsmGpuTest::testCounterRegression()
{
    LinuxDrmUsageSampler sampler;

    QCOMPARE(sampler.sample({{1, 100}}, 10), 0.0);
    QCOMPARE(sampler.sample({{1, 80}}, 10), 0.0);
    QCOMPARE(sampler.sample({{1, 90}}, 10), 0.0);
    QCOMPARE(sampler.sample({{1, 110}}, 10), 100.0);
}

void MsmGpuTest::testElapsedTimeAndReset()
{
    LinuxDrmUsageSampler sampler;

    QCOMPARE(sampler.sample({{1, 100}}, 100), 0.0);
    QCOMPARE(sampler.sample({{1, 150}}, 0), 0.0);
    QCOMPARE(sampler.sample({{1, 150}}, 100), 50.0);

    sampler.reset();
    QCOMPARE(sampler.sample({{1, 1000}}, 100), 0.0);
}

QTEST_GUILESS_MAIN(MsmGpuTest)

#include "msm.moc"
