#include "Link.hpp"
#include <Windows.h>
#include <sddl.h>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <string>
#include <vector>
#include "Log.hpp"

namespace Link {
    namespace {
        std::uint8_t* base = nullptr;
        std::uint32_t overlayFront = 2;
        constexpr std::uint64_t kMcTimeoutMs = 3000;

        template <class T> std::atomic_ref<T> atomic(T& v) { return std::atomic_ref<T>(v); }
        template <class T> T* at(std::uint64_t off) { return reinterpret_cast<T*>(base + off); }

        // This Windows user (plus system and administrators) at normal integrity, so a Minecraft
        // started from the desktop can open it even if MCC runs elevated. Free with LocalFree.
        PSECURITY_DESCRIPTOR sharedWithThisUser() {
            std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
            HANDLE token = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                DWORD size = 0;
                GetTokenInformation(token, TokenUser, nullptr, 0, &size);
                std::vector<std::uint8_t> buf(size);
                LPWSTR sid = nullptr;
                if (size && GetTokenInformation(token, TokenUser, buf.data(), size, &size) &&
                    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
                    sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
                    LocalFree(sid);
                }
                CloseHandle(token);
            }
            sddl += L"S:(ML;;NW;;;ME)";
            PSECURITY_DESCRIPTOR sd = nullptr;
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
            return sd;
        }
    }

    bool create() {
        if (base)
            return true;
        const auto size = proto::kMappingBytes;
        SECURITY_ATTRIBUTES access{ sizeof(access), sharedWithThisUser(), FALSE };
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, access.lpSecurityDescriptor ? &access : nullptr, PAGE_READWRITE,
            DWORD(size >> 32), DWORD(size & 0xFFFFFFFF), proto::kMappingName);
        const DWORD err = GetLastError();
        if (access.lpSecurityDescriptor)
            LocalFree(access.lpSecurityDescriptor);
        if (!mapping) {
            log("CreateFileMapping failed (" + std::to_string(err) + ")");
            return false;
        }
        // ponytail: the mapping handle is never closed; it lives as long as MCC does.
        base = static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!base) {
            log("MapViewOfFile failed (" + std::to_string(GetLastError()) + ")");
            CloseHandle(mapping);
            return false;
        }

        // Minecraft may still hold a mapping from a previous run: reset everything this side owns.
        std::memset(base + proto::kOffSkyState, 0, sizeof(proto::SkyState));
        std::memset(base + proto::kOffOverlayCtl, 0, 0x100);
        std::memset(base + proto::kOffInputRing, 0, proto::kInputRingDataOff);
        std::memset(base + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
        std::memset(base + proto::kOffActorTable, 0, sizeof(proto::ActorTable));
        std::memset(base + proto::kOffEventRing, 0, proto::kEventRingDataOff);
        std::memset(base + proto::kOffWorldEntities, 0, sizeof(proto::WorldEntities));
        std::memset(base + proto::kOffRenderRing, 0, proto::kRenRingDataOff);
        auto* header = at<proto::Header>(proto::kOffHeader);
        header->version = proto::kVersion;
        header->skyrimPid = GetCurrentProcessId();
        header->skyrimHeartbeatMs = GetTickCount64();
        atomic(header->magic).store(proto::kMagic, std::memory_order_release);

        log(std::string("shared memory ") + (err == ERROR_ALREADY_EXISTS ? "reused" : "created") + " (" +
            std::to_string(size >> 20) + " MB)");
        return true;
    }

    bool valid() { return base != nullptr; }

    bool mcAlive() {
        if (!base)
            return false;
        const auto last = atomic(at<proto::Header>(proto::kOffHeader)->mcHeartbeatMs).load(std::memory_order_acquire);
        return last != 0 && GetTickCount64() - last < kMcTimeoutMs;
    }

    void heartbeat() {
        if (base)
            atomic(at<proto::Header>(proto::kOffHeader)->skyrimHeartbeatMs).store(GetTickCount64(), std::memory_order_release);
    }

    // Seqlock: seq is odd while writing.
    void writeSkyState(const proto::SkyState& state) {
        if (!base)
            return;
        auto* dst = at<proto::SkyState>(proto::kOffSkyState);
        auto seq = atomic(dst->seq);
        const auto s = seq.load(std::memory_order_relaxed);
        seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(reinterpret_cast<std::uint8_t*>(dst) + 4, reinterpret_cast<const std::uint8_t*>(&state) + 4, sizeof(state) - 4);
        seq.store(s + 2, std::memory_order_release);
    }

    bool readMcState(proto::McState& out) {
        if (!base)
            return false;
        auto* src = at<proto::McState>(proto::kOffMcState);
        auto seq = atomic(src->seq);
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1) {
                _mm_pause();
                continue;
            }
            std::memcpy(&out, src, sizeof(out));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) == s1)
                return true;
        }
        return false;
    }

    // Byte ring of 8-byte aligned {type, bytes} messages; kColPad means "wrap to the start".
    bool writeCollision(proto::ColType type, const void* payload, std::uint32_t bytes) {
        if (!base)
            return false;
        auto* ring = base + proto::kOffCollisionRing;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kColRingTailOff);
        auto* data = ring + proto::kColRingDataOff;
        constexpr auto size = proto::kColRingDataBytes;
        const std::uint64_t msgBytes = (sizeof(proto::ColMsgHeader) + bytes + 7) & ~7ull;
        if (msgBytes > size / 2)
            return false;
        auto head = atomic(headRef).load(std::memory_order_relaxed);
        const auto tail = atomic(tailRef).load(std::memory_order_acquire);
        auto pos = head % size;
        const auto padBytes = (pos + msgBytes > size) ? size - pos : 0;
        if (size - (head - tail) < msgBytes + padBytes)
            return false;
        if (padBytes) {
            *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { proto::kColPad, 0 };
            head += padBytes;
            pos = 0;
        }
        *reinterpret_cast<proto::ColMsgHeader*>(data + pos) = { type, bytes };
        std::memcpy(data + pos + sizeof(proto::ColMsgHeader), payload, bytes);
        atomic(headRef).store(head + msgBytes, std::memory_order_release);
        return true;
    }

    void pushInput(proto::InputType type, std::uint16_t code, std::int32_t a, std::int32_t b, std::int32_t c) {
        if (!base)
            return;
        auto* ring = base + proto::kOffInputRing;
        auto& headRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingHeadOff);
        auto& tailRef = *reinterpret_cast<std::uint64_t*>(ring + proto::kInputRingTailOff);
        const auto head = atomic(headRef).load(std::memory_order_relaxed);
        if (head - atomic(tailRef).load(std::memory_order_acquire) >= proto::kInputRingEntries)
            return;  // Minecraft isn't draining; drop rather than block the game's window thread
        auto* entry = reinterpret_cast<proto::InputEvent*>(ring + proto::kInputRingDataOff) + (head & (proto::kInputRingEntries - 1));
        *entry = { std::uint16_t(type), code, a, b, c };
        atomic(headRef).store(head + 1, std::memory_order_release);
    }

    // state bits 0-1: middle slot, bit 2: middle holds an unread frame. Swap our front for it.
    bool acquireOverlayFrame() {
        if (!base)
            return false;
        auto& state = at<proto::OverlayCtl>(proto::kOffOverlayCtl)->state;
        if (!(atomic(state).load(std::memory_order_acquire) & proto::kOverlayDirty))
            return false;
        overlayFront = atomic(state).exchange(overlayFront, std::memory_order_acq_rel) & 3;
        return true;
    }

    const std::uint8_t* frontPixels() { return base + proto::kOffOverlayPixels + proto::kOverlaySlotBytes * overlayFront; }

    const proto::OverlaySlotHdr* frontHeader() {
        return at<proto::OverlaySlotHdr>(proto::kOffOverlaySlotHdr + sizeof(proto::OverlaySlotHdr) * overlayFront);
    }
}
