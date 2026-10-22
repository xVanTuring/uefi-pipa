##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = pipa
  PLATFORM_GUID                  = 0965E99C-81B3-4F49-9ABF-A7A39F591528
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/pipaPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = pipaPkg/pipa.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8250
  # 1 = SM8250-AB
  # 2 = SM8250-AC
  #
  SOC_TYPE                       = 2

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Pad 6"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"pipa"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"23043RP34G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"M82"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1800
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2880
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

!if $(TARGET) == DEBUG
  #
  # Debug: an ASSERT prints and returns instead of halting (SiliciumPkg's DEBUG mask 0x2F has
  # DEBUG_PROPERTY_ASSERT_DEADLOOP_ENABLED). RamoopsLogLib restarts into fastboot on the ASSERT line itself, so the
  # mask only matters when that restart fails.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x0F
!endif

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|pipaPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Debug output also in the stock kernel's ramoops console zone (0xB0000000); an ASSERT restarts into fastboot with a
  # warm reset, and UniXNU-pipa's log reader fetches the log
  #
  RamLogLib|pipaPkg/Library/RamoopsLogLib/RamoopsLogLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|pipaPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  pipa/AcpiTables.inf

  #
  # UniXNU: the boot manager, its registrar, the ramdisk block device and the XNU loader
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf

[LibraryClasses]
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
