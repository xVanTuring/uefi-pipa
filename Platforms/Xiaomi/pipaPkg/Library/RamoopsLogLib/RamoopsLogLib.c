/** @file
  RamLogLib for pipa: the debug output is also kept in the stock kernel's ramoops console zone, and an ASSERT restarts the
  tablet into fastboot with a warm reset, so the log reader of UniXNU-pipa (package/linux-log) fetches the whole firmware log
  as /sys/fs/pstore/console-ramoops-0.

  The stock kernel reserves 4 MiB at 0xB0000000 for ramoops (its built-in ramoops_memreserve=4M: a 2 MiB console zone, then a
  2 MiB pmsg zone, no ECC). A zone is a persistent_ram_buffer: the signature "DBGC", the write position and the used size,
  then the ring. The memory survives a PMIC warm reset into fastboot and a `fastboot boot` of the reader (2026-10-22), but not
  a hard reset, which is what the power key and stock firmware restarts give.

  The restart is the one the stock msm-poweroff makes for `reboot bootloader` with qcom,force-warm-reboot: through the SPMI
  arbiter the PM8150 PON's reboot reason (SOFT_RB_SPARE bits 7:1, bootloader = 2) and its PS_HOLD reset type (warm), the IMEM
  restart reason, the two SCM calls (secure watchdog debug boot part off, PMIC arbiter halted), then PS_HOLD deasserted. The
  arbiter code is UniXNU-tb128fu's package/BootResetCore.h with the version 5 arbiter of the pipa device tree
  (qcom,spmi@c440000: core, chnls 0xC600000, obsrvr 0xE600000, cnfg 0xC40A000, EE 0); every address lies in the AOSS region of
  the memory map. Until the MMU is on (the first SEC lines) all of it is device memory, so the log is written byte by byte.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/RamLogLib.h>
#include <Library/TimerLib.h>

#define RAMOOPS_CONSOLE_BASE  0xB0000000
#define RAMOOPS_CONSOLE_SIZE  0x200000
#define RAMOOPS_SIGNATURE     0x43474244                      // "DBGC" (PERSISTENT_RAM_SIG); the console zone's own signature is 0
#define RAMOOPS_HEADER_SIZE   12
#define RAMOOPS_DATA_SIZE     (RAMOOPS_CONSOLE_SIZE - RAMOOPS_HEADER_SIZE)

#define SPMI_CORE             0x0C440000
#define SPMI_CHNLS            0x0C600000
#define SPMI_OBSRVR           0x0E600000
#define SPMI_CNFG             0x0C40A000
#define SPMI_EE               0
#define SPMI_VERSION_V5       0x50000000
#define SPMI_VERSION_V7       0x70000000
#define SPMI_FEATURES         0x04
#define SPMI_MAX_APIDS        512
#define SPMI_APID_MAP         0x900                           // core + 4 * APID: bits 19:8 the PPID
#define SPMI_CNFG_OWNER       0x700                           // cnfg + 4 * APID: bits 2:0 the EE that may write it
#define SPMI_OBS_EE_STRIDE    0x10000
#define SPMI_OBS_APID_STRIDE  0x80
#define SPMI_CHNL_APID_STRIDE 0x10000
#define SPMI_CMD              0x00
#define SPMI_STATUS           0x08
#define SPMI_WDATA0           0x10
#define SPMI_RDATA0           0x18
#define SPMI_OP_EXT_WRITEL    0
#define SPMI_OP_EXT_READL     1
#define SPMI_STATUS_DONE      BIT0
#define SPMI_STATUS_ERRORS    0x0E
#define SPMI_POLLS            100000

#define PON_PPID              0x008                           // PMIC SID 0, peripheral 0x08: the PM8150 PON at 0x800
#define PON_TYPE              0x04
#define PON_SUBTYPE           0x05
#define PON_PERIPH_TYPE       0x01
#define PON_GEN2_PRIMARY      0x04
#define PON_GEN2_SECONDARY    0x05
#define PON_PS_HOLD_RST_CTL   0x5A
#define PON_PS_HOLD_RST_CTL2  0x5B
#define PON_SOFT_RB_SPARE     0x8F
#define PON_RESET_EN          BIT7
#define PON_RESET_TYPE_MASK   0x0F
#define PON_WARM_RESET        0x01
#define PON_REASON_MASK       0xFE
#define PON_REASON_BOOTLOADER (2 << 1)

#define PS_HOLD               0x0C264000                      // restart@c264000, pshold-base
#define IMEM_RESTART_REASON   0x146BF65C                      // qcom,msm-imem-restart_reason
#define RESTART_BOOTLOADER    0x77665500

#define SCM_WDOG_DEBUG_OFF    0xC2000109                      // SCM_SIP_FNID (SCM_SVC_BOOT, SCM_WDOG_DEBUG_BOOT_PART), atomic, SMC64
#define SCM_HALT_PMIC_ARB     0xC2000901                      // SCM_SIP_FNID (SCM_SVC_PWR, SCM_IO_DISABLE_PMIC_ARBITER)
#define PSCI_SYSTEM_RESET     0x84000009

STATIC BOOLEAN  mRestarting;

STATIC
VOID
AppendByte (
  IN UINT8  Byte)
{
  UINT32 Start = MmioRead32 (RAMOOPS_CONSOLE_BASE + 4);
  UINT32 Size  = MmioRead32 (RAMOOPS_CONSOLE_BASE + 8);

  // Start over when something else left a Header behind
  if (MmioRead32 (RAMOOPS_CONSOLE_BASE) != RAMOOPS_SIGNATURE || Start >= RAMOOPS_DATA_SIZE || Size > RAMOOPS_DATA_SIZE) {
    RamLogReset ();
    Start = 0;
    Size  = 0;
  }

  MmioWrite8 (RAMOOPS_CONSOLE_BASE + RAMOOPS_HEADER_SIZE + Start, Byte);

  // Ring: the oldest Bytes go once the Zone is full
  Start = (Start + 1 == RAMOOPS_DATA_SIZE) ? 0 : Start + 1;
  if (Size < RAMOOPS_DATA_SIZE) {
    Size++;
  }

  MmioWrite32 (RAMOOPS_CONSOLE_BASE + 4, Start);
  MmioWrite32 (RAMOOPS_CONSOLE_BASE + 8, Size);
}

STATIC
VOID
AppendText (
  IN CONST CHAR8  *Text)
{
  while (*Text != '\0') {
    AppendByte ((UINT8)*Text++);
  }
}

STATIC
VOID
AppendHex (
  IN UINT32  Value)
{
  for (INTN Shift = 28; Shift >= 0; Shift -= 4) {
    AppendByte ((UINT8)"0123456789ABCDEF"[(Value >> Shift) & 0xF]);
  }
}

STATIC
UINT32
SpmiCommand (
  IN UINT8  Opcode,
  IN UINT8  Address)
{
  // One Byte (byte count 0) at Address of the Peripheral
  return ((UINT32)Opcode << 27) | ((UINT32)Address << 4);
}

STATIC
EFI_STATUS
SpmiWait (
  IN UINTN  Channel)
{
  for (UINTN Poll = 0; Poll < SPMI_POLLS; Poll++) {
    UINT32 Status = MmioRead32 (Channel + SPMI_STATUS);
    if (Status & SPMI_STATUS_DONE) {
      return (Status & SPMI_STATUS_ERRORS) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SpmiRead8 (
  IN  UINTN   Observer,
  IN  UINT8   Address,
  OUT UINT8  *Value)
{
  MmioWrite32 (Observer + SPMI_CMD, SpmiCommand (SPMI_OP_EXT_READL, Address));

  EFI_STATUS Status = SpmiWait (Observer);
  if (!EFI_ERROR (Status)) {
    *Value = (UINT8)MmioRead32 (Observer + SPMI_RDATA0);
  }

  return Status;
}

STATIC
EFI_STATUS
SpmiWrite8 (
  IN UINTN  Channel,
  IN UINT8  Address,
  IN UINT8  Value)
{
  MmioWrite32 (Channel + SPMI_WDATA0, Value);
  MmioWrite32 (Channel + SPMI_CMD, SpmiCommand (SPMI_OP_EXT_WRITEL, Address));

  return SpmiWait (Channel);
}

STATIC
EFI_STATUS
SpmiUpdate8 (
  IN UINTN  Observer,
  IN UINTN  Channel,
  IN UINT8  Address,
  IN UINT8  Mask,
  IN UINT8  Value)
{
  UINT8      Old;
  EFI_STATUS Status = SpmiRead8 (Observer, Address, &Old);

  if (!EFI_ERROR (Status)) {
    Status = SpmiWrite8 (Channel, Address, (UINT8)((Old & ~Mask) | (Value & Mask)));
  }

  return Status;
}

//
// The PON's reboot reason and PS_HOLD reset type; a Code for the Log, 0 when both were written.
//
STATIC
UINT32
PrepareWarmFastboot (VOID)
{
  UINT32 Version = MmioRead32 (SPMI_CORE);
  UINT32 Apids   = MmioRead32 (SPMI_CORE + SPMI_FEATURES) & 0x7FF;
  INTN   Apid    = -1;
  UINT8  Type    = 0;
  UINT8  Subtype = 0;

  if (Version < SPMI_VERSION_V5 || Version >= SPMI_VERSION_V7) {
    return 0x10;
  }

  if (Apids == 0 || Apids > SPMI_MAX_APIDS) {
    return 0x11;
  }

  // As the mainline driver picks it: the first Mapping of the PON, replaced by one this EE owns
  for (UINT32 Index = 0; Index < Apids; Index++) {
    UINT32 Map = MmioRead32 (SPMI_CORE + SPMI_APID_MAP + 4 * Index);
    if (Map == 0 || ((Map >> 8) & 0xFFF) != PON_PPID) {
      continue;
    }

    if (Apid < 0 || (MmioRead32 (SPMI_CNFG + SPMI_CNFG_OWNER + 4 * Index) & 7) == SPMI_EE) {
      Apid = Index;
    }
  }

  if (Apid < 0) {
    return 0x12;
  }

  if ((MmioRead32 (SPMI_CNFG + SPMI_CNFG_OWNER + 4 * Apid) & 7) != SPMI_EE) {
    return 0x13;
  }

  UINTN Observer = SPMI_OBSRVR + SPMI_OBS_EE_STRIDE * SPMI_EE + SPMI_OBS_APID_STRIDE * Apid;
  UINTN Channel  = SPMI_CHNLS + SPMI_CHNL_APID_STRIDE * Apid;

  // Only a gen2 PON keeps the Reason in SOFT_RB_SPARE with this Encoding
  if (EFI_ERROR (SpmiRead8 (Observer, PON_TYPE, &Type)) || EFI_ERROR (SpmiRead8 (Observer, PON_SUBTYPE, &Subtype))) {
    return 0x20;
  }

  if (Type != PON_PERIPH_TYPE || (Subtype != PON_GEN2_PRIMARY && Subtype != PON_GEN2_SECONDARY)) {
    return 0x21;
  }

  if (EFI_ERROR (SpmiUpdate8 (Observer, Channel, PON_SOFT_RB_SPARE, PON_REASON_MASK, PON_REASON_BOOTLOADER))) {
    return 0x30;
  }

  // As qpnp_pon_system_pwr_off: Reset off, at least 10 Sleep Clock Cycles, the Type, Reset on
  if (EFI_ERROR (SpmiUpdate8 (Observer, Channel, PON_PS_HOLD_RST_CTL2, PON_RESET_EN, 0))) {
    return 0x40;
  }

  MicroSecondDelay (500);

  if (EFI_ERROR (SpmiUpdate8 (Observer, Channel, PON_PS_HOLD_RST_CTL, PON_RESET_TYPE_MASK, PON_WARM_RESET))) {
    return 0x41;
  }

  if (EFI_ERROR (SpmiUpdate8 (Observer, Channel, PON_PS_HOLD_RST_CTL2, PON_RESET_EN, PON_RESET_EN))) {
    return 0x42;
  }

  return 0;
}

STATIC
VOID
RestartIntoFastboot (VOID)
{
  UINTN ArgInfo, Arg0, Arg1;

  mRestarting = TRUE;

  UINT32 Code = PrepareWarmFastboot ();
  AppendText ("RamLog: restarting into fastboot, PON 0x");
  AppendHex (Code);
  AppendText (Code == 0 ? " (warm)\n" : " (not prepared, the log may be lost)\n");

  MmioWrite32 (IMEM_RESTART_REASON, RESTART_BOOTLOADER);

  ArgInfo = 2;
  Arg0    = 1;
  Arg1    = 0;
  ArmCallSmc3 (SCM_WDOG_DEBUG_OFF, &ArgInfo, &Arg0, &Arg1);

  ArgInfo = 1;
  Arg0    = 0;
  ArmCallSmc3 (SCM_HALT_PMIC_ARB, &ArgInfo, &Arg0, NULL);

  MmioWrite32 (PS_HOLD, 0);
  MicroSecondDelay (1000000);

  // Still here: PSCI's Reset, a hard one unless the PON Type above holds
  AppendText ("RamLog: PS_HOLD did not restart, PSCI SYSTEM_RESET\n");
  ArmCallSmc0 (PSCI_SYSTEM_RESET, NULL, NULL, NULL);

  CpuDeadLoop ();
}

STATIC
BOOLEAN
StartsWith (
  IN CONST UINT8 *Buffer,
  IN UINTN        NumberOfBytes,
  IN CONST CHAR8 *Prefix)
{
  for (UINTN Index = 0; Prefix[Index] != '\0'; Index++) {
    if (Index >= NumberOfBytes || Buffer[Index] != (UINT8)Prefix[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

VOID
EFIAPI
RamLogReset (VOID)
{
  MmioWrite32 (RAMOOPS_CONSOLE_BASE,     RAMOOPS_SIGNATURE);
  MmioWrite32 (RAMOOPS_CONSOLE_BASE + 4, 0);
  MmioWrite32 (RAMOOPS_CONSOLE_BASE + 8, 0);
}

VOID
EFIAPI
RamLogWrite (
  IN CONST UINT8 *Buffer,
  IN UINTN        NumberOfBytes)
{
  for (UINTN Index = 0; Index < NumberOfBytes; Index++) {
    AppendByte (Buffer[Index]);
  }

  // An ASSERT Line: DebugAssert's "ASSERT [Module] File(Line): ...", a vendor binary's "ASSERT File +Line: ..." and the status
  // code handler's "\n\rDXE_ASSERT!: ...". Not "ASSERT_EFI_ERROR (Status = ...)", the Line printed before one.
  if (!mRestarting && (StartsWith (Buffer, NumberOfBytes, "ASSERT ") || StartsWith (Buffer, NumberOfBytes, "\n\rDXE_ASSERT!"))) {
    RestartIntoFastboot ();
  }
}
