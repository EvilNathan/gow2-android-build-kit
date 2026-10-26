// GoW2-Port: overrides de God of War II (SCUS-97481 v1.01, SHA1 E6A62A0F3E781C29513F563F4625B9B3A9103C56).
// Las direcciones solo valen para ese ELF. Modelo: referencia/gow1/overrides/gow_overrides.cpp (Waffle565, vía KIexster).
// Cada override responde a un bloqueo apuntado en docs/NOTES.md ("Blockers").
#include "runtime/ee_scheduler.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_perf.h" // setGameFramePc: FPS reales en el contador F10
#include "ps2_host_backend.h" // raylib: IsGamepadAvailable / IsKeyDown para el teclado
#include <ps2_recompiled_functions.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    // ---- Diagnóstico (solo con variables de entorno; no cambia el comportamiento) ----

    // GOW2_BOOT_TRACE=1: cuenta las llamadas a las funciones de la secuencia de arranque (0x188700…) y las
    // imprime cada segundo, para ver en qué bucle de espera se queda.
    struct Gow2BootCall
    {
        uint32_t address;
        const char *name;
        PS2Runtime::RecompiledFunction original;
        std::atomic<uint32_t> count{0};
        std::atomic<int32_t> lastV0{0};
    };
    Gow2BootCall g_gow2BootCalls[] = {
        {0x002D4C88u, "sceSifRebootIop", FUN_002d4c88_0x2d4c88},
        {0x002D4C38u, "sceSifSyncIop", FUN_002d4c38_0x2d4c38},
        {0x002B6900u, "sceCdInit", FUN_002b6900_0x2b6900},
        {0x002B6CC0u, "sceCdMmode", FUN_002b6cc0_0x2b6cc0},
        {0x002B67A8u, "cdCheckSCmd", FUN_002b67a8_0x2b67a8},
        {0x002D27B0u, "sceSifInitRpc", FUN_002d27b0_0x2d27b0},
        {0x002D42D8u, "sceSifInitIopHeap", FUN_002d42d8_0x2d42d8},
    };

    template <size_t I>
    void gow2BootTraceCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Gow2BootCall &call = g_gow2BootCalls[I];
        call.original(rdram, ctx, runtime);
        call.count.fetch_add(1u, std::memory_order_relaxed);
        call.lastV0.store(static_cast<int32_t>(GPR_U32(ctx, 2)), std::memory_order_relaxed);
    }

    template <size_t... I>
    void gow2InstallBootTrace(PS2Runtime &runtime, std::index_sequence<I...>)
    {
        (runtime.replaceFunction(g_gow2BootCalls[I].address, gow2BootTraceCall<I>), ...);
    }

    std::string gow2GuestString(const uint8_t *rdram, uint32_t addr)
    {
        std::string text;
        for (uint32_t i = 0; addr != 0u && i < 128u; ++i)
        {
            const char c = static_cast<char>(rdram[(addr + i) & 0x01FFFFFFu]);
            if (c == '\0')
                break;
            text.push_back((c >= 0x20 && c < 0x7F) ? c : '?');
        }
        return text;
    }

    // 0x2D4A98 sceSifLoadModule(path, argLen, args): lo usa el cargador del arranque (0x1885F8) para 8 módulos
    void gow2TraceLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<int> logged{0};
        const std::string path = gow2GuestString(rdram, GPR_U32(ctx, 4));
        const std::string args = gow2GuestString(rdram, GPR_U32(ctx, 6));
        FUN_002d4a98_0x2d4a98(rdram, ctx, runtime);
        if (logged.fetch_add(1) < 64)
            std::fprintf(stderr, "[gow2-boot] sceSifLoadModule(\"%s\", args=\"%s\") -> %d\n", path.c_str(), args.c_str(),
                         static_cast<int32_t>(GPR_U32(ctx, 2)));
    }

    // 0x2D2DF8 sceSifBindRpc(client, sid, mode): devuelve <0 si falla; client+0x24 (server) queda a 0 si no existe
    void gow2TraceBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static std::atomic<int> logged{0};
        const uint32_t client = GPR_U32(ctx, 4);
        const uint32_t sid = GPR_U32(ctx, 5);
        FUN_002d2df8_0x2d2df8(rdram, ctx, runtime);
        uint32_t server = 0;
        std::memcpy(&server, rdram + ((client + 0x24u) & 0x01FFFFFFu), sizeof(server));
        if (logged.fetch_add(1) < 32)
            std::fprintf(stderr, "[gow2-boot] sceSifBindRpc(client=%08x, sid=%08x) -> %d server=%08x\n", client, sid,
                         static_cast<int32_t>(GPR_U32(ctx, 2)), server);
    }

    // GOW2_THREAD_WATCH=<s>: cada <s> segundos, los hilos del EE (estado, espera, pc, ra) y, con GOW2_BOOT_TRACE,
    // los contadores de arranque. Para bloqueos silenciosos (como GOW_THREAD_WATCH en GoW1).
    void gow2StartWatch(PS2Runtime &runtime, double period, bool bootTrace)
    {
        std::thread([&runtime, period, bootTrace] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(period));
                const EeKernelSnapshot snapshot = runtime.eeScheduler().snapshot();
                std::fprintf(stderr, "[gow2-watch] running=%d cycle=%llu\n", snapshot.runningThreadId,
                             static_cast<unsigned long long>(snapshot.eeCycle));
                for (const EeThreadSnapshot &t : snapshot.threads)
                {
                    std::fprintf(stderr, "[gow2-watch]   thread id=%d prio=%d status=%d wait=%d/%d pc=%08x ra=%08x sp=%08x\n", t.id,
                                 t.currentPriority, int(t.status), int(t.waitReason), t.waitId, t.pc, t.ra, t.sp);
                    if (t.pc == 0x0027D7E8u) // bucle de lista de 0x27D788: [sp] = cabeza, [sp+4] = iterador, nodo: +0 next, +8 objeto
                    {
                        const uint8_t *ram = runtime.memory().getRDRAM();
                        auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4); return v; };
                        const uint32_t head = word(t.sp), it = word(t.sp + 4u);
                        std::fprintf(stderr, "[gow2-watch]     list head=%08x it=%08x nodes:", head, it);
                        uint32_t node = word(head);
                        for (int i = 0; i < 8; ++i, node = word(node))
                            std::fprintf(stderr, " %08x(obj=%08x)", node, word(node + 8u));
                        std::fprintf(stderr, "\n");
                        // 0x18D5BC: obj22 = [0x362CA0]; estado = obj22->vtable[0x94]() → [0x2D814C]; si estado, push
                        const uint32_t obj22 = word(0x00362CA0u), vt = obj22 ? word(obj22 + 0x20u) : 0u;
                        std::fprintf(stderr, "[gow2-watch]     obj22=%08x vtable=%08x vt[0x94]=%08x adj=%d state=[0x2D814C]=%08x\n",
                                     obj22, vt, vt ? word(vt + 0x94u) : 0u,
                                     vt ? static_cast<int16_t>(word(vt + 0x90u) & 0xFFFFu) : 0, word(0x002D814Cu));
                    }
                }
                {
                    // árbol de 0x183330: centinela [0x2D7FC0], base del pool de nodos [0x2D7FC8] (hijos = índices de 16 bits)
                    const uint8_t *ram = runtime.memory().getRDRAM();
                    auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4); return v; };
                    const uint32_t sentinel = word(0x002D7FC0u), pool = word(0x002D7FC8u);
                    std::fprintf(stderr, "[gow2-watch]   tree sentinel=%08x pool=%08x distance=0x%x heap=[%08x,%08x)\n", sentinel, pool,
                                 sentinel - pool, runtime.guestHeapBase(), runtime.guestHeapLimit());
                }
                // GOW2_PEEK=dir,dir,…: palabras de memoria del EE (hex) en cada pasada, para seguir variables del juego
                if (const char *peek = std::getenv("GOW2_PEEK"))
                {
                    const uint8_t *ram = runtime.memory().getRDRAM();
                    std::fprintf(stderr, "[gow2-watch]   peek");
                    for (const char *cursor = peek; cursor && *cursor;)
                    {
                        char *end = nullptr;
                        const uint32_t address = static_cast<uint32_t>(std::strtoul(cursor, &end, 16)) & 0x01FFFFFCu;
                        uint32_t value = 0;
                        std::memcpy(&value, ram + address, sizeof(value));
                        std::fprintf(stderr, " [%06x]=%08x", address, value);
                        cursor = (end && *end == ',') ? end + 1 : nullptr;
                    }
                    std::fprintf(stderr, "\n");
                }
                if (bootTrace)
                    for (const Gow2BootCall &call : g_gow2BootCalls)
                        std::fprintf(stderr, "[gow2-watch]   boot %-18s %08x calls=%u last_v0=%d\n", call.name, call.address,
                                     call.count.load(), call.lastV0.load());
            }
        }).detach();
    }

    // GOW2_CALL_PROFILE=<s>: envuelve todas las funciones del juego y, cada <s> segundos, imprime las 25 más llamadas en
    // ese intervalo. Sirve cuando GOW2_THREAD_WATCH da siempre el mismo pc (el pc del código recompilado solo se
    // actualiza en algunos puntos): las funciones del bucle en el que está el juego salen arriba.
    constexpr uint32_t kGow2ProfileBase = 0x00100000u;
    constexpr uint32_t kGow2ProfileEnd = 0x002FC000u; // final de .text (0x2FBDA8) redondeado
    constexpr size_t kGow2ProfileSlots = (kGow2ProfileEnd - kGow2ProfileBase) / 4u;
    PS2Runtime::RecompiledFunction g_gow2ProfileOriginals[kGow2ProfileSlots];
    std::atomic<uint32_t> g_gow2ProfileCounts[kGow2ProfileSlots];

    void gow2ProfileWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const size_t slot = (ctx->pc - kGow2ProfileBase) / 4u;
        g_gow2ProfileCounts[slot].fetch_add(1u, std::memory_order_relaxed);
        g_gow2ProfileOriginals[slot](rdram, ctx, runtime);
    }

    void gow2StartCallProfile(PS2Runtime &runtime, double period)
    {
        unsigned hooked = 0;
        // hasFunction primero: lookupFunction escribe un error (con traza) por cada dirección sin función exacta
        for (uint32_t address = kGow2ProfileBase; address < kGow2ProfileEnd; address += 4u)
            if (!runtime.hasFunction(address))
                continue;
            else if (const PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address))
            {
                g_gow2ProfileOriginals[(address - kGow2ProfileBase) / 4u] = original;
                runtime.replaceFunction(address, gow2ProfileWrapper);
                ++hooked;
            }
        std::fprintf(stderr, "[gow2-prof] %u funciones envueltas\n", hooked);
        std::thread([period] {
            static uint32_t previous[kGow2ProfileSlots];
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(period));
                std::pair<uint32_t, uint32_t> top[25]{}; // (llamadas, dirección)
                for (size_t slot = 0; slot < kGow2ProfileSlots; ++slot)
                {
                    const uint32_t now = g_gow2ProfileCounts[slot].load(std::memory_order_relaxed);
                    const uint32_t delta = now - previous[slot];
                    previous[slot] = now;
                    if (delta > top[24].first)
                    {
                        top[24] = {delta, kGow2ProfileBase + static_cast<uint32_t>(slot) * 4u};
                        std::sort(std::begin(top), std::end(top), [](const auto &a, const auto &b) { return a.first > b.first; });
                    }
                }
                std::fprintf(stderr, "[gow2-prof] llamadas en los últimos %.0f s:", period);
                for (const auto &[calls, address] : top)
                    if (calls != 0u)
                        std::fprintf(stderr, " %06x:%u", address, calls);
                std::fprintf(stderr, "\n");
            }
        }).detach();
    }

    // GOW2_MOVIE_TRACE=1: por qué el reproductor de vídeos deja una película. Su bucle (0x191620–0x191828, estado en
    // [0x2D8190]) sale si 0x25EF40() ≠ 0, si [[0x2D7A24]+0x5AC] == 2, si 0x191AB0 (siguiente imagen) devuelve 0, si
    // 0x191BD0 (¿terminada?: [película+0x154] ≠ 0 o, en el modo 2 de libmpeg, sceMpegIsEnd 0x2AC058) da 1, o por un
    // botón cuando la película se puede saltar ([0x2D8194]). Envuelve esas funciones e imprime cada cambio de resultado
    // con el estado de la película ([0x337700]: +0x50 imágenes, +0x54 mostradas, +0x140 modo, +0x14C, +0x154 fin).
    struct Gow2MovieProbe
    {
        uint32_t address;
        const char *name;
        PS2Runtime::RecompiledFunction original;
        std::atomic<uint32_t> calls;
        std::atomic<uint32_t> lastV0;
    };
    Gow2MovieProbe g_gow2MovieProbes[] = {
        {0x00191AB0u, "frame", nullptr, {0u}, {0xDEADBEEFu}},   {0x00191BD0u, "done", nullptr, {0u}, {0xDEADBEEFu}},
        {0x0025EF40u, "quit25ef40", nullptr, {0u}, {0xDEADBEEFu}}, {0x002AC058u, "sceMpegIsEnd", nullptr, {0u}, {0xDEADBEEFu}},
        {0x0018FE08u, "check18fe08", nullptr, {0u}, {0xDEADBEEFu}}, {0x002A89D0u, "spuPos5b", nullptr, {0u}, {0xDEADBEEFu}},
        {0x00190488u, "adpcmFeed", nullptr, {0u}, {0xDEADBEEFu}}};

    void gow2MovieProbeWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        Gow2MovieProbe *probe = nullptr;
        for (Gow2MovieProbe &p : g_gow2MovieProbes)
            if (p.address == ctx->pc)
                probe = &p;
        if (!probe || !probe->original)
            return;
        probe->original(rdram, ctx, runtime);
        const uint32_t calls = probe->calls.fetch_add(1u) + 1u;
        const uint32_t v0 = GPR_U32(ctx, 2);
        if (probe->lastV0.exchange(v0) == v0 && calls % 2000u != 0u)
            return;
        auto word = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; };
        const uint32_t movie = word(0x00337700u), game = word(0x002D7A24u);
        std::fprintf(stderr,
                     "[gow2-movie] %s=%d (llamada %u) estado=%u saltable=%u saltada=%u game+5ac=%d | película %08x: imágenes=%u "
                     "mostradas=%u modo=%u 14c=%x fin154=%u\n",
                     probe->name, static_cast<int32_t>(v0), calls, word(0x002D8190u), word(0x002D8194u), word(0x002D8198u),
                     game ? static_cast<int32_t>(word(game + 0x5ACu)) : -1, movie, movie ? word(movie + 0x50u) : 0u,
                     movie ? word(movie + 0x54u) : 0u, movie ? word(movie + 0x140u) : 0u, movie ? word(movie + 0x14Cu) : 0u,
                     movie ? word(movie + 0x154u) : 0u);
    }

    // El callback del audio de la película (0x1929E0, ver NOTES.md): las primeras entregas, con su longitud, los primeros
    // bytes y el estado del anillo antes y después ([película+0x138] bytes de cabecera, +0x104 lectura, +0x108 lleno).
    PS2Runtime::RecompiledFunction g_gow2MovieAudioOriginal = nullptr;
    void gow2MovieAudioProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned calls = 0;
        auto word = [rdram](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFCu), 4); return v; };
        const uint32_t movie = word(0x00337700u), cb = GPR_U32(ctx, 5);
        const uint32_t data = word(cb + 8u), len = word(cb + 12u);
        const uint32_t before138 = word(movie + 0x138u), beforeFill = word(movie + 0x108u);
        g_gow2MovieAudioOriginal(rdram, ctx, runtime);
        if (++calls <= 12u || calls % 500u == 0u)
        {
            const uint8_t *p = rdram + (data & 0x01FFFFFFu);
            std::fprintf(stderr, "[gow2-movie-audio] #%u len=%u bytes=%02x%02x%02x%02x %02x%02x%02x%02x hdr %u->%u fill %u->%u rd=%u v0=%d\n",
                         calls, len, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], before138, word(movie + 0x138u), beforeFill,
                         word(movie + 0x108u), word(movie + 0x104u), static_cast<int32_t>(GPR_U32(ctx, 2)));
        }
    }

    // 0x190190(staging, size, spuOffset): lo que se envía a la SPU2; las marcas (byte 1) de cada bloque ADPCM de 16 bytes.
    PS2Runtime::RecompiledFunction g_gow2MovieSpuOriginal = nullptr;
    void gow2MovieSpuProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static unsigned calls = 0;
        if (++calls <= 24u)
        {
            const uint32_t src = GPR_U32(ctx, 4), size = GPR_U32(ctx, 5), dst = GPR_U32(ctx, 6);
            char flags[16 * 3 + 1] = {};
            for (uint32_t b = 0; b < 16u && b * 16u < size; ++b)
                std::snprintf(flags + b * 3u, 4u, "%02x ", rdram[(src + b * 16u + 1u) & 0x01FFFFFFu]);
            std::fprintf(stderr, "[gow2-movie-spu] #%u src=%x size=%x dst=%x flags: %s\n", calls, src, size, dst, flags);
        }
        g_gow2MovieSpuOriginal(rdram, ctx, runtime);
    }

    void gow2StartMovieTrace(PS2Runtime &runtime)
    {
        if ((g_gow2MovieSpuOriginal = runtime.lookupFunction(0x00190190u)))
            runtime.replaceFunction(0x00190190u, gow2MovieSpuProbe);
        for (Gow2MovieProbe &probe : g_gow2MovieProbes)
            if (runtime.hasFunction(probe.address) && (probe.original = runtime.lookupFunction(probe.address)))
                runtime.replaceFunction(probe.address, gow2MovieProbeWrapper);
        if ((g_gow2MovieAudioOriginal = runtime.lookupFunction(0x001929E0u)))
            runtime.replaceFunction(0x001929E0u, gow2MovieAudioProbe);
        std::fprintf(stderr, "[gow2-movie] traza del reproductor de vídeos activada\n");
    }

    // ---- Overrides ----

    // Sector donde empieza la capa 1 del DVD-9 de GoW II (game/disc/.disc_layout, "layer1"; docs/NOTES.md).
    constexpr uint32_t kGow2Layer1StartLbn = 2083872u;

    void gow2WriteGuest32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (addr == 0u)
            return;
        std::memcpy(rdram + (addr & 0x01FFFFFFu), &value, sizeof(value));
    }

    // 0x2B6BF0 int sceCdReadDvdDualInfo(int *on_dual, unsigned int *layer1_start)
    // El original primero comprueba que el servidor de comandos S del CDVD (0x2B67A8) esté listo y, si no,
    // devuelve 0 sin llamar al IOP. El arranque (0x188738) la repite mientras devuelva 0, así que se quedaba
    // ahí para siempre (bloqueo 1 de la fase 2; en GoW1, el mismo con su gowCdReadDvdDualInfo).
    // Se responde como el lector de una consola: disco de doble capa y dónde empieza la capa 1.
    void gow2CdReadDvdDualInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        gow2WriteGuest32(rdram, GPR_U32(ctx, 4), 1u);
        gow2WriteGuest32(rdram, GPR_U32(ctx, 5), kGow2Layer1StartLbn);
        SET_GPR_U32(ctx, 2, 1u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x2D4850 int sceSifLoadStartModuleBuffer(iopAddr, argLen, args, int *result)
    // El arranque (0x1887E4 → 0x2BF660) copia a la memoria del IOP un módulo mínimo que va dentro del ELF (0x2DB590,
    // ~0x360 bytes) y lo arranca con esta función; luego exige id >= 0 y (*result & ~3) == 0, o se cuelga a propósito
    // en 0x18D5A0 (bloqueo 5 de la fase 2). El runtime aún no ejecuta módulos cargados desde la memoria del IOP, así
    // que se responde como una consola: cargado (id válido) y terminado con NO_RESIDENT_END (1). Como en GoW1.
    void gow2LoadStartModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        gow2WriteGuest32(rdram, GPR_U32(ctx, 7), 1u); // NO_RESIDENT_END
        SET_GPR_U32(ctx, 2, 0x40001000u);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x2B56F0 sceIpuInit (bloqueo 11). El stub genérico del runtime (Kernel/Stubs/IPU.cpp) lleva direcciones fijas de
    // otro juego: llama a SetD4_CHCR en 0x126428 y carga las tablas de cuantización de la IPU desde 0x1721E0/0x172230,
    // que en GoW II son otra cosa. Con ellas, al empezar el primer vídeo la cadena de DMA del VIF1 se desincronizaba
    // ("[vif1] unhandled VIFcode", etiquetas basura) y el juego esperaba para siempre el FINISH del GS (0x18ADD0);
    // con este override, el vídeo se reproduce y el juego sigue leyendo el mando.
    // Adaptado de gowIpuInit de GoW1 (referencia/gow1/overrides/gow_overrides.cpp, Waffle565 vía KIexster, GPLv3). La
    // secuencia es la del sceIpuInit de GoW II (decodificada de sus lq/sq): tabla IQ en 0x2D9550 (GoW1 0x2A1610) con los
    // qwords 0,1,2,3,4,4,4,4, y tabla VQ en 0x2D95A0 (GoW1 0x2A1660).
    void gow2IpuInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &memory = runtime->memory();
        memory.write32(0x1000B400u, 1u);        // D4_CHCR: lo que hace SetD4_CHCR (0x2B5678)
        memory.write32(0x10002010u, 0x40000000u); // IPU_CTRL: reset
        memory.write32(0x10002000u, 0u);          // IPU_CMD: BCLR
        for (uint32_t offset : {0u, 16u, 32u, 48u, 64u, 64u, 64u, 64u})
            memory.write128(0x10007010u, runtime->Load128(rdram, ctx, 0x002D9550u + offset));
        memory.write32(0x10002000u, 0x50000000u); // SETIQ (intra)
        memory.write32(0x10002000u, 0x58000000u); // SETIQ (no intra)
        for (uint32_t offset : {0u, 16u})
            memory.write128(0x10007010u, runtime->Load128(rdram, ctx, 0x002D95A0u + offset));
        memory.write32(0x10002000u, 0x60000000u); // SETVQ
        memory.write32(0x10002000u, 0x90000000u); // SETTH
        memory.write32(0x10002010u, 0x40000000u);
        memory.write32(0x10002000u, 0u);
        std::fprintf(stderr, "[gow2-ipu] sceIpuInit con las tablas de SCUS-97481\n");
        SET_GPR_S32(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    // 0x190A08: la función de espera del reproductor de vídeos. El juego la registra en libmpeg (0x2A9400) y libmpeg la
    // llama (0x2A9418) en sus esperas activas: 0x2A8EA0 mete la imagen en la cola de pantalla (2 huecos) y da vueltas
    // `while (cola llena)` hasta que el VBlank saca una; esta función demultiplexa más datos en cada vuelta. En la PS2,
    // cada vuelta cuesta su tiempo; aquí cuesta más en el PC de lo que avanza en ciclos del EE, y la película (y el
    // tiempo emulado, con sus VBlank) iba a ~0,57× (34 VBlank por segundo; EE ocupado el 99 %, sin esperas).
    // catchUpToHost añade los ciclos que faltan cuando el tiempo emulado se queda detrás del reloj real, como hace GoW1
    // con su sondeo de la película (gowMovieStreamService, 0x188B80; referencia/gow1/overrides/gow_overrides.cpp,
    // Waffle565 vía KIexster, GPLv3).
    void gow2MovieIdle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->eeScheduler().catchUpToHost(EeScheduler::kEeClockHz / 60u);
        FUN_00190a08_0x190a08(rdram, ctx, runtime);
    }

    // ---- Mando: libpad2 en el EE ----
    // Adaptado de GoW1 (referencia/gow1/overrides/gow_overrides.cpp y gow_pad2_packet.h, Waffle565 vía KIexster, GPLv3).
    // God of War II lee el mando en el EE con libpad2 (sockets y un paquete de 18 bytes), que habla con el driver
    // DBCMAN del IOP; sin estos overrides, el juego no ve ningún mando y se queda en el aviso "no hay mando conectado".
    // Se responde con el mando nativo del runtime (PSPadBackend: un mando del PC o, si no hay, el teclado).
    // Direcciones: libpad2 de GoW1 + 0x3C0F0 (el mismo desplazamiento que libcdvd/libdbc); cada una empieza una función
    // de config/funcmap.csv con el tamaño de GoW1 (±4) y el juego las llama desde 0x188B4C/0x188BC8/0x188C5C.
    struct Gow2PadSocket
    {
        bool open = false;
        uint32_t port = 0;
    };
    std::array<Gow2PadSocket, 2> g_gow2PadSockets{};

    void gow2PadReturn(R5900Context *ctx, uint32_t result)
    {
        SET_GPR_S32(ctx, 2, static_cast<int32_t>(result));
        ctx->pc = GPR_U32(ctx, 31);
    }

    Gow2PadSocket *gow2PadSocket(R5900Context *ctx)
    {
        const uint32_t handle = GPR_U32(ctx, 4);
        return handle < g_gow2PadSockets.size() && g_gow2PadSockets[handle].open ? &g_gow2PadSockets[handle] : nullptr;
    }

    uint8_t *gow2PadBuffer(uint8_t *rdram, uint32_t address, size_t size)
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return address && physical < 0x02000000u && size <= 0x02000000u - physical ? rdram + physical : nullptr;
    }

    // 0x2B78D8 scePad2Init / 0x2B7918 scePad2End
    void gow2Pad2Init(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_gow2PadSockets = {};
        gow2PadReturn(ctx, 1u);
    }

    // 0x2B7980 int scePad2CreateSocket(scePad2SocketParam *param, void *dmaBuffer): param = {?, port, slot}
    void gow2Pad2CreateSocket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint8_t *params = gow2PadBuffer(rdram, GPR_U32(ctx, 4), 12);
        uint32_t port = 0, slot = 0;
        if (params)
        {
            std::memcpy(&port, params + 4, 4);
            std::memcpy(&slot, params + 8, 4);
        }
        if (!params || port >= 2u || slot != 0u || (GPR_U32(ctx, 5) & 63u))
        {
            gow2PadReturn(ctx, 0xFFFFFFFFu);
            return;
        }
        for (uint32_t handle = 0; handle < g_gow2PadSockets.size(); ++handle)
            if (!g_gow2PadSockets[handle].open)
            {
                g_gow2PadSockets[handle] = {true, port};
                std::fprintf(stderr, "[gow2-pad2] socket=%u port=%u\n", handle, port);
                gow2PadReturn(ctx, handle);
                return;
            }
        gow2PadReturn(ctx, 0xFFFFFFFFu);
    }

    // 0x2B7A88 scePad2DeleteSocket(socket)
    void gow2Pad2DeleteSocket(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        Gow2PadSocket *socket = gow2PadSocket(ctx);
        if (socket)
            socket->open = false;
        gow2PadReturn(ctx, socket ? 1u : 0xFFFFFFFFu);
    }

    // 0x2B7C88 scePad2GetState(socket): 1 = estable. Solo el puerto 1 tiene mando (el del PC o el teclado).
    void gow2Pad2GetState(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const Gow2PadSocket *socket = gow2PadSocket(ctx);
        gow2PadReturn(ctx, socket && socket->port == 0u ? 1u : 0u);
    }

    // 0x2B7BB8 scePad2GetButtonProfile(socket, u8 *profile): 0xFF... = DualShock 2 (todos los botones)
    void gow2Pad2GetButtonProfile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint8_t *buffer = gow2PadBuffer(rdram, GPR_U32(ctx, 5), 4);
        const Gow2PadSocket *socket = gow2PadSocket(ctx);
        if (!buffer || !socket || socket->port != 0u)
        {
            gow2PadReturn(ctx, 0xFFFFFFFFu);
            return;
        }
        std::memset(buffer, 0xFF, 4);
        gow2PadReturn(ctx, 4u);
    }

    // GOW2_PAD_SCRIPT (diagnóstico): pulsaciones a tiempos fijos, en segundos desde la primera lectura del mando, para
    // repetir una secuencia sin teclado (como GOW_PAD_GUION en GoW1). Elementos separados por comas:
    //   "T:boton"         pulsa en T (0,3 s)
    //   "T1-T2/P:boton"   pulsa cada P segundos entre T1 y T2
    //   "T1-T2:boton"     mantiene pulsado entre T1 y T2
    // Botones: start, select, x, o, sq, tri, up, down, left, right, l1, r1, l2, r2; stick izquierdo a tope: lup, ldown,
    // lleft, lright. Ej.: "20:start,25-90/3:x,130-160:lup" (andar hacia delante 30 s).
    struct Gow2PadPress
    {
        double from, to, period;
        uint32_t mask;
    };

    // bits 16-19 de la máscara del guion: el stick izquierdo
    constexpr uint32_t kGow2StickUp = 0x10000u, kGow2StickDown = 0x20000u, kGow2StickLeft = 0x40000u, kGow2StickRight = 0x80000u;

    std::vector<Gow2PadPress> gow2ParsePadScript(const char *text)
    {
        static const std::pair<const char *, uint32_t> kButtons[] = {
            {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080},
            {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800}, {"tri", 0x1000}, {"o", 0x2000}, {"x", 0x4000},
            {"sq", 0x8000}, {"lup", kGow2StickUp}, {"ldown", kGow2StickDown}, {"lleft", kGow2StickLeft}, {"lright", kGow2StickRight}};
        std::vector<Gow2PadPress> presses;
        std::string item;
        for (const char *c = text;; ++c)
        {
            if (*c != ',' && *c != '\0')
            {
                item.push_back(*c);
                continue;
            }
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                Gow2PadPress press{0.0, 0.0, 0.0, 0u};
                const std::string when = item.substr(0, colon), name = item.substr(colon + 1u);
                char *end = nullptr;
                press.from = press.to = std::strtod(when.c_str(), &end);
                if (end && *end == '-')
                    press.to = std::strtod(end + 1, &end);
                if (end && *end == '/')
                    press.period = std::strtod(end + 1, &end);
                for (const auto &[button, mask] : kButtons)
                    if (name == button)
                        press.mask = mask;
                if (press.mask != 0u)
                    presses.push_back(press);
            }
            item.clear();
            if (*c == '\0')
                break;
        }
        return presses;
    }

    // Botones del guion pulsados ahora (bits activos a 1; 16-19, el stick izquierdo)
    uint32_t gow2PadScriptButtons()
    {
        static const char *text = std::getenv("GOW2_PAD_SCRIPT");
        if (!text)
            return 0u;
        static const std::vector<Gow2PadPress> presses = gow2ParsePadScript(text);
        static const auto start = std::chrono::steady_clock::now();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        constexpr double kHold = 0.3;
        uint32_t pressed = 0u;
        for (const Gow2PadPress &press : presses)
        {
            if (now < press.from || now > press.to + kHold)
                continue;
            const double into = now - press.from;
            const bool held = press.period <= 0.0 && press.to > press.from; // "T1-T2:boton": todo el intervalo
            const double phase = press.period > 0.0 ? into - press.period * static_cast<long>(into / press.period) : into;
            if (held || phase < kHold)
                pressed |= press.mask;
        }
        static uint32_t last = 0u;
        if (pressed != last)
            std::fprintf(stderr, "[gow2-pad2:script] t=%.1f s botones=%05x\n", now, pressed);
        last = pressed;
        return pressed;
    }

    // 0x2B7AE0 scePad2Read(socket, u8 *data): 18 bytes = botones (activos a 0, orden del cable), RX/RY/LX/LY y las 12
    // presiones. Sin mando en el PC, el teclado: botones según PSPadBackend (flechas, Z/X/C/V, Q/E, 1/3, Intro = Start)
    // y sticks con WASD (izquierdo) e IJKL (derecho).
    void gow2Pad2Read(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // 16:9, as in GoW1: the runtime always presents a 16:9 image, and GoW II renders 4:3 unless its own
        // widescreen option is on. Data part of PCSX2's SCUS-97481_2F123FD8.pnach "Widescreen 16:9" (CRASHARKI &
        // nemesis2000): the game's widescreen flag (its Options menu setting) and three aspect floats (16/9), written
        // every frame because loading a save restores the saved options. The pnach's Hor+ instruction (0x234A48) is
        // applied by the recompiler: [patches] in config/recomp.template.toml.
        // GOW_ASPECT=4:3 (the launcher's "4:3 original") writes what the game's own options setter (0x1E1A08) writes
        // with widescreen off: flag 0 and 4/3. The patched instruction then computes the same as the original one
        // (it only differs when 0x363810 is not 4/3), so no regeneration is needed to switch.
        static const bool widescreen = [] { const char *v = std::getenv("GOW_ASPECT"); return !(v && std::strcmp(v, "4:3") == 0); }();
        rdram[0x002D86D8u] = widescreen ? 1u : 0u;
        for (uint32_t address : {0x0032F300u, 0x0032F338u, 0x00363810u})
        {
            const uint32_t aspect = widescreen ? 0x3FE38E39u : 0x3FAAAAABu; // 16/9 : 4/3
            std::memcpy(rdram + address, &aspect, sizeof(aspect));
        }

        uint8_t *buffer = gow2PadBuffer(rdram, GPR_U32(ctx, 5), 18);
        const Gow2PadSocket *socket = gow2PadSocket(ctx);
        uint8_t state[32]{};
        if (!buffer || !socket || socket->port != 0u || !runtime || !runtime->padBackend().readState(0, 0, state, sizeof(state)))
        {
            gow2PadReturn(ctx, 0xFFFFFFFFu);
            return;
        }
        if (!IsGamepadAvailable(0) && !IsGamepadAvailable(1) && !IsGamepadAvailable(2) && !IsGamepadAvailable(3))
        {
            state[6] = IsKeyDown(KEY_A) ? 0 : IsKeyDown(KEY_D) ? 255 : 128;
            state[7] = IsKeyDown(KEY_W) ? 0 : IsKeyDown(KEY_S) ? 255 : 128;
            state[4] = IsKeyDown(KEY_J) ? 0 : IsKeyDown(KEY_L) ? 255 : 128;
            state[5] = IsKeyDown(KEY_I) ? 0 : IsKeyDown(KEY_K) ? 255 : 128;
        }
        if (const uint32_t scripted = gow2PadScriptButtons())
        {
            state[2] &= static_cast<uint8_t>(~scripted);
            state[3] &= static_cast<uint8_t>(~(scripted >> 8));
            if (scripted & kGow2StickUp) state[7] = 0;
            if (scripted & kGow2StickDown) state[7] = 255;
            if (scripted & kGow2StickLeft) state[6] = 0;
            if (scripted & kGow2StickRight) state[6] = 255;
        }
        uint8_t packet[18]{};
        for (size_t i = 0; i < 6; ++i)
            packet[i] = state[i + 2];
        const uint16_t buttons = static_cast<uint16_t>(state[2] | (state[3] << 8));
        constexpr uint16_t kPressureBits[] = {0x20, 0x80, 0x10, 0x40, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800, 0x100, 0x200};
        for (size_t i = 0; i < 12; ++i)
            packet[6 + i] = (buttons & kPressureBits[i]) ? 0 : 255;
        std::memcpy(buffer, packet, sizeof(packet));
        static uint16_t lastButtons = 0xFFFFu;
        static unsigned reads = 0;
        if (++reads <= 3u || buttons != lastButtons)
            std::fprintf(stderr, "[gow2-pad2] read buttons=%04x\n", buttons);
        lastButtons = buttons;
        gow2PadReturn(ctx, 18u);
    }

    void applyGow2Overrides(PS2Runtime &runtime)
    {
        const bool bootTrace = std::getenv("GOW2_BOOT_TRACE") != nullptr;
        if (bootTrace)
        {
            gow2InstallBootTrace(runtime, std::make_index_sequence<std::size(g_gow2BootCalls)>{});
            runtime.replaceFunction(0x002D4A98u, gow2TraceLoadModule);
            runtime.replaceFunction(0x002D2DF8u, gow2TraceBindRpc);
        }
        if (const char *watch = std::getenv("GOW2_THREAD_WATCH"))
            gow2StartWatch(runtime, std::max(1.0, std::atof(watch)), bootTrace);

        // El juego crea su heap principal (0x146318, antes de main) desde el final del .bss hasta 0x01FF7FF8. El heap
        // interno del runtime nacía en la misma zona (con el SetupHeap del crt0: [0x37F4D0, 0x1F00000)), así que lo que
        // reservaban los stubs pisaba las estructuras del juego: el "RB NODE Heap" daba nodos en 0x84000010 y la búsqueda
        // de 0x183330 no terminaba nunca (bloqueo 3 de la fase 2). Como en GoW1, a la memoria libre bajo el ELF (0x100000).
        runtime.setPrivateGuestHeap(0x000A0000u, 0x000FF000u);

        const bool dualInfo = runtime.replaceFunction(0x002B6BF0u, gow2CdReadDvdDualInfo);
        const bool moduleBuffer = runtime.replaceFunction(0x002D4850u, gow2LoadStartModuleBuffer);
        // 0x1875E0 (de dónde arrancó el juego, según argv) ya no se sustituye: el SetupThread del runtime rellena los
        // argumentos de main como una consola (argc = 1, argv[0] = "cdrom0:\SCUS_974.81;1"; antes era un TEMPORAL).
        std::fprintf(stderr, "[gow2-override] sceCdReadDvdDualInfo=%d moduleBuffer=%d\n", dualInfo, moduleBuffer);

        // mando (libpad2, ver arriba)
        const bool pad = runtime.replaceFunction(0x002B78D8u, gow2Pad2Init) &&         // scePad2Init
                         runtime.replaceFunction(0x002B7918u, gow2Pad2Init) &&         // scePad2End
                         runtime.replaceFunction(0x002B7980u, gow2Pad2CreateSocket) && // scePad2CreateSocket
                         runtime.replaceFunction(0x002B7A88u, gow2Pad2DeleteSocket) && // scePad2DeleteSocket
                         runtime.replaceFunction(0x002B7AE0u, gow2Pad2Read) &&         // scePad2Read
                         runtime.replaceFunction(0x002B7BB8u, gow2Pad2GetButtonProfile) &&
                         runtime.replaceFunction(0x002B7C88u, gow2Pad2GetState);
        std::fprintf(stderr, "[gow2-override] libpad2=%d\n", pad);
        const bool ipu = runtime.replaceFunction(0x002B56F0u, gow2IpuInit); // sceIpuInit (bloqueo 11)
        const bool movieIdle = runtime.replaceFunction(0x00190A08u, gow2MovieIdle); // espera de los vídeos
        std::fprintf(stderr, "[gow2-override] sceIpuInit=%d movieIdle=%d\n", ipu, movieIdle);

        // Contador F10: GoW II dibuja todos los frames en un solo búfer de pantalla (sin flips) y el GS recibe un frame
        // por VBlank haga lo que haga el juego, así que marcaba 60 siempre. 0x14E380 entrega cada frame al GS, una vez
        // por frame del juego: el F10 cuenta sus entradas ("[G]"). Es lo mismo que GOW_PERF_FRAME_PC=0x14E380.
        ps2_perf::setGameFramePc(0x0014E380u);

        if (std::getenv("GOW2_MOVIE_TRACE"))
            gow2StartMovieTrace(runtime);

        // al final, para envolver también los overrides de arriba
        if (const char *profile = std::getenv("GOW2_CALL_PROFILE"))
            gow2StartCallProfile(runtime, std::max(1.0, std::atof(profile)));
    }
}

PS2_REGISTER_GAME_OVERRIDE("God of War II (SCUS-97481)", "SCUS_974.81", 0x00100008u, 0u, applyGow2Overrides)
