// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 bbport contributors (deadinside28/bloodborne_pc)
// SPDX-License-Identifier: GPL-2.0-or-later

// Machine code for hooks in the game's own code (Bloodborne's frame rate and GX draw hooks),
// from bbport 0.7 (https://github.com/deadinside28/bloodborne_pc): a stub is written into the
// trampoline area next to the game's code, and a jump to it replaces instructions at the hooked
// place.

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>

#include "common/types.h"

namespace Core::BbGuestCode {

/// A stub's bytes, built in place.
struct Emitter {
    std::array<u8, 512> code{};
    std::size_t size = 0;
    bool overflow = false;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            if (size < code.size()) {
                code[size] = b;
            }
            ++size;
        }
        overflow = overflow || size > code.size();
    }
    void U64(u64 v) {
        for (int i = 0; i < 8; ++i) {
            Bytes({u8(v >> (i * 8))});
        }
    }
    /// Saves the caller-saved general registers and xmm0-7 (arguments at a function's entry),
    /// aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});                               // push r12 (the unaligned stack)
        Bytes({0x49, 0x89, 0xe4});                         // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0});                   // and rsp, -16
        Bytes({0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00}); // sub rsp, 128
        for (u8 x = 0; x < 8; ++x) {                       // movups [rsp + 16x], xmm<x>
            Bytes({0x0f, 0x11, u8(0x44 | (x << 3)), 0x24, u8(x * 16)});
        }
    }
    void RestoreVolatile() {
        for (u8 x = 0; x < 8; ++x) { // movups xmm<x>, [rsp + 16x]
            Bytes({0x0f, 0x10, u8(0x44 | (x << 3)), 0x24, u8(x * 16)});
        }
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    /// jmp [rip]: to `target` anywhere.
    void JmpAbs(u64 target) {
        Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});
        U64(target);
    }
};

} // namespace Core::BbGuestCode
