// ============================================================
//  mgapi.cpp — bsnes-plus side of the microgarbage cart runtime.
//
//  Loads mgapi.dll via LoadLibrary, resolves its exports, and
//  routes cart-bus reads + audio + reset through it. The DLL
//  itself contains the entire microgarbage runtime; this file is
//  the bsnes-side adapter, modeled after MSU1.
//
//  Public domain (CC0). No warranty.
// ============================================================
#include <snes.hpp>

#define MGAPI_CPP
namespace SNES {

Mgapi mgapi;

#include "serialization.cpp"

// Matches MgapiConfig in include/mgapi/mgapi.h. Keep this in sync
// when the ABI bumps. We copy it locally so this file doesn't need
// the mgapi headers checked into bsnes-plus.
struct MgapiResetConfigABI {
  uint32_t hold_ms;
};
struct MgapiConfigABI {
  uint32_t    cart_window_size;
  uint32_t    audio_sample_rate;
  uint32_t    audio_frames_max;
  uint16_t    tcp_listen_port;
  uint8_t     pad_count;
  uint8_t     rom_select;
  const char *shell_elf_path;
  const char *autostart_path;
  MgapiResetConfigABI reset;
  uint8_t     disable_default_stdio;
  uint8_t     _reserved_pad[7];
};

#if defined(_WIN32)
  #include <windows.h>
  typedef HMODULE DllHandle;
  static DllHandle dll_open(const char *path)        { return ::LoadLibraryA(path); }
  static void      dll_close(DllHandle h)            { ::FreeLibrary(h); }
  static void     *dll_sym(DllHandle h, const char *n) { return (void *)::GetProcAddress(h, n); }
#else
  #include <dlfcn.h>
  typedef void *DllHandle;
  static DllHandle dll_open(const char *path)        { return ::dlopen(path, RTLD_NOW); }
  static void      dll_close(DllHandle h)            { ::dlclose(h); }
  static void     *dll_sym(DllHandle h, const char *n) { return ::dlsym(h, n); }
#endif

template <typename F>
static bool resolve(DllHandle h, const char *name, F &out) {
  void *p = dll_sym(h, name);
  if(!p) {
    fprintf(stderr, "mgapi: missing export '%s'\n", name);
    return false;
  }
  // GetProcAddress returns a FARPROC; bouncing through void* keeps
  // the strict-aliasing rules happy for both MSVC and mingw.
  out = reinterpret_cast<F>(p);
  return true;
}

bool Mgapi::try_load() {
  if(dll_handle) return true;
  DllHandle h = dll_open("mgapi.dll");
  if(!h) {
#if defined(_WIN32)
    fprintf(stderr,
            "mgapi: LoadLibrary(\"mgapi.dll\") FAILED, GetLastError=%lu\n",
            (unsigned long)::GetLastError());
#else
    fprintf(stderr, "mgapi: dlopen(\"mgapi.dll\") FAILED: %s\n", dlerror());
#endif
    fprintf(stderr,
            "mgapi: cart bus will fall through to the loaded .sfc bytes;\n"
            "  the smoke ROM's 65816 menu will appear instead of the mgapi runtime.\n"
            "  fix: make sure mgapi.dll + libgcc_s_seh-1.dll + libwinpthread-1.dll\n"
            "  are present alongside bsnes.exe.\n");
    fflush(stderr);
    return false;
  }

  bool ok =
    resolve(h, "mgapi_init",              p_init)              &&
    resolve(h, "mgapi_shutdown",          p_shutdown)          &&
    resolve(h, "mgapi_cart_read",         p_cart_read)         &&
    resolve(h, "mgapi_post_joypads",      p_post_joypads)      &&
    resolve(h, "mgapi_step",              p_step)              &&
    resolve(h, "mgapi_audio_pull",        p_audio_pull)        &&
    resolve(h, "mgapi_version",           p_version)           &&
    resolve(h, "mgapi_cart_reset_begin",  p_cart_reset_begin)  &&
    resolve(h, "mgapi_cart_reset_ready",  p_cart_reset_ready)  &&
    resolve(h, "mgapi_cart_reset_end",    p_cart_reset_end);
  if(!ok) {
    dll_close(h);
    return false;
  }

  // Optional export (older DLLs lack it): the port-2 mouse forward for the
  // cursor overlay. Resolved directly so a missing symbol is silent, not a
  // scary "missing export" line — the runtime stays fully functional without it.
  p_post_mouse = reinterpret_cast<decltype(p_post_mouse)>(dll_sym(h, "mgapi_post_mouse"));

  MgapiConfigABI cfg = {};
  cfg.cart_window_size  = 64 * 1024;
  cfg.audio_sample_rate = 0;        // 0 = auto-detect (WASAPI default endpoint)
  cfg.audio_frames_max  = 4096;
  cfg.tcp_listen_port   = 2323;
  cfg.pad_count         = 2;
  cfg.rom_select        = 0;        // MGAPI_ROM_SMOKE
  cfg.shell_elf_path    = nullptr;
  cfg.autostart_path    = nullptr;
  cfg.reset.hold_ms     = 50;       // CIC-equivalent + window stabilization
  cfg.disable_default_stdio = 1;    // shell exits cleanly; PuTTY on :2323

  int rc = p_init(&cfg);
  if(rc != 0) {
    fprintf(stderr, "mgapi: mgapi_init failed: %d\n", rc);
    dll_close(h);
    return false;
  }
  dll_handle = h;
  fprintf(stderr, "mgapi: loaded (%s)\n", p_version());
  return true;
}

// ----------------------------------------------------------------
//  Memory interface
// ----------------------------------------------------------------

uint8 Mgapi::read(unsigned addr) {
  if(!dll_handle) return cpu.regs.mdr;   // open-bus when not loaded
  return p_cart_read(addr & 0xFFFFFF);
}

void Mgapi::write(unsigned, uint8) {
  // Cart bus is read-only: SNES writes here are open-bus (the
  // cart kernel never writes to its own ROM space anyway).
}

unsigned Mgapi::size() const {
  // 16 MB virtual span (whole HiROM region) so map_xml's mirror
  // arithmetic doesn't wrap unexpectedly. The actual backing is
  // the 64 KB window inside mgapi.dll; the mirror is implicit.
  return 0x1000000;
}

// ----------------------------------------------------------------
//  Coprocessor + audio stream
// ----------------------------------------------------------------

void Mgapi::Enter() { mgapi.enter(); }

// Pack one port's worth of joypad button bits into the SNES auto-
// joypad word ($4218) shape: bit 15 = B, ..., bit 4 = R, bits 3..0
// = controller-type signature (0 for standard pad). Uses the public
// Input::port_read API — same path the SNES kernel hits via $4016
// reads, but driven from the C++ mapper side. The caller MUST run
// SNES::input.poll() first (resets the per-port counter and refreshes
// the Qt-side mapper cache); then 16 calls to port_read return the
// 12 button bits + 4 signature bits in standard SNES order.
//
// Note: system.interface->input_poll(...) would let us query each
// button directly without the bit-bang dance, but System::interface
// is private and Mgapi isn't a friend of System. port_read is public
// and reaches the same Qt mapper via input.cpp's port_read switch
// case for Device::Joypad. Functionally equivalent.
static uint16_t mgapi_pack_joypad_port(bool port_index) {
  uint16_t w = 0;
  for(unsigned i = 0; i < 16; i++) {
    w = (uint16_t)((w << 1) | (SNES::input.port_read(port_index) & 1u));
  }
  return w;
}

void Mgapi::enter() {
  while(true) {
    scheduler.synchronize();

    int16 left = 0, right = 0;
    if(dll_handle) {
      // Pull one stereo frame from mgapi's ring. Underrun → silence.
      int16_t buf[2] = {0, 0};
      uint32_t got = p_audio_pull(buf, 1);
      if(got > 0) { left = buf[0]; right = buf[1]; }

      // Advance mgapi's runtime once per 60 Hz frame's worth of
      // audio samples (735 ≈ 44100 / 60). Drives the VM scheduler,
      // audio service render, TCP listener poll, etc. Joypad post
      // happens here too so its cadence matches mgapi_step()'s.
      if(samples_until_step == 0) {
        // Refresh the Qt-side input cache + reset port counters
        // before reading individual buttons.
        SNES::input.poll();
        uint16_t pads[4] = {0, 0, 0, 0};
        pads[0] = mgapi_pack_joypad_port(false);   // port 1 (pad: Start exits)
        // Port 2 is read as a SNES Mouse below (32-bit serial), NOT packed as a
        // pad, so pads[1] stays 0. Pads 2/3 unused (no multitap path yet).
        p_post_joypads(pads);

        // Port 2 = SNES Mouse: clock out its 32-bit report and forward dx/dy +
        // L/R to the cursor overlay. port_read(true)'s per-port counter was
        // reset by poll() and is untouched (port 2 wasn't packed as a pad), so
        // reads return mouse cases 0..31. If port 2 is actually a pad, these are
        // just pad bits → no meaningful cursor motion.
        if(p_post_mouse) {
          unsigned b[32];
          for(unsigned i = 0; i < 32; i++) b[i] = SNES::input.port_read(true) & 1u;
          int xmag = (b[25]<<6)|(b[26]<<5)|(b[27]<<4)|(b[28]<<3)|(b[29]<<2)|(b[30]<<1)|b[31];
          int ymag = (b[17]<<6)|(b[18]<<5)|(b[19]<<4)|(b[20]<<3)|(b[21]<<2)|(b[22]<<1)|b[23];
          int dx = b[24] ? -xmag : xmag;   // bit 24 = X direction (1 = negative)
          int dy = b[16] ? -ymag : ymag;   // bit 16 = Y direction (1 = negative)
          unsigned btn = (b[9] ? 1u : 0u) | (b[8] ? 2u : 0u);  // bit9=Left, bit8=Right
          p_post_mouse(dx, dy, btn);
        }

        p_step(16666666ull);
        samples_until_step = 735;
      } else {
        samples_until_step--;
      }
    }

    sample(left, right);
    step(1);
    synchronize_cpu();
  }
}

// ----------------------------------------------------------------
//  Lifecycle (matches MSU1's hook points)
// ----------------------------------------------------------------

void Mgapi::init() {
  // Nothing — try_load handles real work; init runs even when the
  // cart isn't an mgapi cart, so it stays inert.
}

void Mgapi::enable() {
  audio.add_stream(this);
  audio_frequency(44100.0);
}

void Mgapi::unload() {
  if(dll_handle) {
    p_shutdown();
    dll_close((DllHandle)dll_handle);
    dll_handle = nullptr;
  }
}

void Mgapi::power() { reset(); }

void Mgapi::reset() {
  create(Mgapi::Enter, 44100);
  samples_until_step = 0;

  if(dll_handle) {
    // Real-hardware-style reset: hold the cart bus while mgapi
    // restages the window and the CIC-equivalent timer elapses.
    p_cart_reset_begin();
    while(!p_cart_reset_ready()) {
      p_step(1000000ull);   // 1 ms of mgapi-side runtime per spin
    }
    p_cart_reset_end();
  }
}

}  // namespace SNES
