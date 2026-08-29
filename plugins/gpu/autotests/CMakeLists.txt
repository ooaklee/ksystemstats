# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2023 David Redondo <kde@david-redondo.de>

ecm_add_test(nvidia.cpp ../NvidiaSmiProcess.cpp
    TEST_NAME nvidiatest
    LINK_LIBRARIES Qt::Test
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ecm_add_test(msm.cpp ../LinuxDrmFdInfo.cpp
        TEST_NAME msmtest
        LINK_LIBRARIES Qt::Test
    )
endif()
