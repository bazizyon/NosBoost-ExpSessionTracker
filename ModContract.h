#pragma once
#include <cstdint>
#include <imgui.h>

// Plain C contract between NosBoostRuntime and mods
// Function pointers resolved by name via GetProcAddress.
// This header plus sdk/classes/ and sdk/packets/ is everything a mod needs;
// it never depends on anything inside NosBoostRuntime/.

struct TLBSWidget;

// Build one of these per SDK class a mod depends on, straight from that
// class's own metadata. Checked exactly: any version mismatch, or a size
// mismatch against the live game, breaks the mod -- no partial credit.
struct ModClassRequirement {
    const char* ClassName;
    uint32_t RequiredVersion;
    uint32_t RequiredSize;
};

struct TickContext {
    bool isPlayerLoaded;
};

namespace Packet {
    using ErasedPacketHandler = bool(*)(const void* Raw, void* HandlerPtr);

    template <typename PacketT>
    bool  PacketTrampoline(const void* Raw, void* HandlerPtr) {
        using HandlerFn = bool(*)(const PacketT&);
        return reinterpret_cast<HandlerFn>(HandlerPtr)(*static_cast<const PacketT*>(Raw));
    }
}

// A mod's own self-reported health, via ModHost::ReportStatus -- separate
// from the runtime's automatic ModClassRequirement check above. Ok/Warning
// is the mod's own judgment call (e.g. an optional dependency is
// missing); a mod can also self-report Broken if it's decided it's not
// worth running without something it needs. Check and report every tick
// -- whatever a mod last reported is what the F9 menu shows.
enum class ModHealthLevel : uint32_t {
    Ok,
    Warning,
    Broken,
};

// Handed to the mod once, in ModStartup -- never cache across a reload.
// Send and receive are separate header namespaces; subscribe to both if a
// mod cares about both directions.
//
// Worked example -- overriding c_mode's wings:
//
//   bool OnCMode(const Packet::CModePacket& Packet) {
//       Packet::CModePacket Replacement = Packet;
//       Replacement.wingType = MyDesiredWingType;
//       Packet::InjectReceivePacket(Host, Replacement); // runtime serializes it
//       return false; // suppress the real one
//   }
//   // in ModStartup: Packet::SubscribePacket(Host, &OnCMode);
//
// Two mods overriding the SAME packet isn't arbitrated -- whichever
// injects last wins, same as two mods hooking the same function.
//
// Worked example -- calling another mod's exported function:
//
//   #include "ExampleModAPI.h" // the OTHER mod's own public header
//   const auto Fn = reinterpret_cast<CalculateExampleDamageFn>(
//       Host->GetModExport("ExampleMod", "CalculateExampleDamage"));
//   if (Fn) { const uint32_t Damage = Fn(&Input); }
//
// Resolve this where you actually use it (ModTick, a button press) --
// never cache the pointer, and never resolve it from inside your own
// ModStartup, since mod load order is arbitrary and the other mod may
// not exist yet at that point.
struct ModHost {
    void(* SubscribePacket)(const char* Header, Packet::ErasedPacketHandler Trampoline, void* HandlerPtr);
    void(* UnsubscribePacket)(const char* Header, void* HandlerPtr);
    void(* SubscribeSentPacket)(const char* Header, Packet::ErasedPacketHandler Trampoline, void* HandlerPtr);
    void(* UnsubscribeSentPacket)(const char* Header, void* HandlerPtr);

    void(* InjectSendPacket)(const char* Header, const void* Packet);
    void(* InjectReceivePacket)(const char* Header, const void* Packet);

    // Live vtable for a class in ClassRegistry::KnownClasses, or 0 if not found.
    // Go through Widget::Create<T>() below instead of calling this directly.
    uintptr_t(* ResolveVTable)(const char* ClassName);

    // Raw function pointer from another mod's DLL by name, or nullptr if
    // that mod isn't found/started, or doesn't export that name. Cast to
    // the function type declared in that mod's own public API header.
    void*(* GetModExport)(const char* ModName, const char* FunctionName);

    // This mod's own self-reported health, shown in the F9 menu -- see
    // ModHealthLevel above.
    void(* ReportStatus)(ModHealthLevel Level, const char* Message);
};

namespace Packet {
    template <typename PacketT>
    void SubscribePacket(const ModHost* Host, bool(* Handler)(const PacketT&)) {
        Host->SubscribePacket(PacketT::Header, &PacketTrampoline<PacketT>, reinterpret_cast<void*>(Handler));
    }

    template <typename PacketT>
    void UnsubscribePacket(const ModHost* Host, bool(* Handler)(const PacketT&)) {
        Host->UnsubscribePacket(PacketT::Header, reinterpret_cast<void*>(Handler));
    }

    template <typename PacketT>
    void SubscribeSentPacket(const ModHost* Host, bool(* Handler)(const PacketT&)) {
        Host->SubscribeSentPacket(PacketT::Header, &PacketTrampoline<PacketT>, reinterpret_cast<void*>(Handler));
    }

    template <typename PacketT>
    void UnsubscribeSentPacket(const ModHost* Host, bool(* Handler)(const PacketT&)) {
        Host->UnsubscribeSentPacket(PacketT::Header, reinterpret_cast<void*>(Handler));
    }

    template <typename PacketT>
    void InjectSendPacket(const ModHost* Host, const PacketT& Packet) {
        Host->InjectSendPacket(PacketT::Header, &Packet);
    }

    template <typename PacketT>
    void InjectReceivePacket(const ModHost* Host, const PacketT& Packet) {
        Host->InjectReceivePacket(PacketT::Header, &Packet);
    }
}

namespace Widget {
    // T is deduced from the call site (e.g. Widget::Create<TEWLabel>(Host))
    // -- T::ClassName is what gets resolved, never a hand-typed string.
    // Constructs T with `new`, in the mod's own module; nullptr if T's
    // vtable wasn't found live (see ClassRegistry::KnownClasses).
    template <typename T>
    T* Create(const ModHost* Host) {
        const uintptr_t VTable = Host->ResolveVTable(T::ClassName);
        return VTable ? new T(VTable) : nullptr;
    }
}

// --- exports every mod DLL must provide, by exact name ---
//
// const ModClassRequirement* ModGetRequirements(size_t* OutCount);
//     Called before ModStartup. Nothing to declare -> nullptr, *OutCount = 0.
//
// void ModStartup(ImGuiContext*, ImGuiMemAllocFunc, ImGuiMemFreeFunc, void*, const ModHost*);
//     Called once per load. Set up ImGui (SetCurrentContext/
//     SetAllocatorFunctions) if the mod draws UI, and subscribe to packets here.
//
// void ModShutdown();
//     Called once before unload, only if ModStartup ran for this load.
//
// void ModTick(TLBSWidget* RootWidget);
//     Once per tick while started. RootWidget may be null -- check first.
//
// void ModToggleMainWindow();
//     Called on the mod's F9-menu click -- flip your own visibility flag.
using ModGetRequirementsFn = const ModClassRequirement*(*)(size_t*);
using ModStartupFn = void(*)(ImGuiContext*, ImGuiMemAllocFunc, ImGuiMemFreeFunc, void*, const ModHost*);
using ModShutdownFn = void(*)();
using ModTickFn = void(__cdecl*)(TLBSWidget*, TickContext);
using ModToggleMainWindowFn = void(*)();
