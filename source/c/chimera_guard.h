// chimera_guard.h
// Shared between kernel driver (chimera_guard.c) and user-mode (ChimeraMain.cpp).
// No includes needed — IOCTL value is pre-computed to work in both contexts.
#pragma once

// Device path opened from user mode
#define CHIMERA_DEVICE_PATH  L"\\\\.\\ChimeraGuard"

// IOCTL: send ULONG pid → driver protects that pid from TerminateProcess
// Pre-computed: CTL_CODE(0x8000, 0x800, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0)
// = (0x8000<<16)|(0<<14)|(0x800<<2)|0 = 0x80002000
#define IOCTL_CHIMERA_PROTECT  0x80002000UL
