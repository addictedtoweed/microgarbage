// ============================================================
//  mgapi.hpp — microgarbage cart runtime, bsnes-plus side.
//
//  Loads mgapi.dll (which contains the full microgarbage VM +
//  audio + trashfs + SNES cart-window decode + TCP shell). When
//  active, every cart-bus read in HiROM space is routed through
//  mgapi_cart_read, and audio is pulled from mgapi_audio_pull.
//
//  Public domain (CC0). No warranty.
// ============================================================
class Mgapi : public Memory, public Coprocessor, public Stream {
public:
  static void Enter();
  void enter();

  // Lifecycle (parallel to MSU1's hook points). init() runs once;
  // enable() is called after cart load when has_mgapi is true;
  // power() and reset() bracket emulator resets; unload() runs at
  // cart eject.
  void init();
  void enable();
  void power();
  void reset();
  void unload();

  // Try to load mgapi.dll from the bsnes binary's working dir.
  // Returns true on success; once loaded, the DLL stays resident
  // for the lifetime of the bsnes process.
  bool try_load();
  bool is_loaded() const { return dll_handle != nullptr; }

  // Memory interface — the bus calls this for any address in our
  // mapped HiROM ranges. Cart bus is read-only; write is a no-op.
  uint8 read(unsigned addr);
  void  write(unsigned addr, uint8 data);
  unsigned size() const;

  void serialize(serializer&);

private:
  void *dll_handle;

  // mgapi.dll function pointers (resolved at try_load time).
  int      (*p_init)(const void *);
  void     (*p_shutdown)();
  uint8_t  (*p_cart_read)(uint32_t);
  void     (*p_post_joypads)(const uint16_t *);
  void     (*p_step)(uint64_t);
  uint32_t (*p_audio_pull)(int16_t *, uint32_t);
  const char *(*p_version)();
  void     (*p_cart_reset_begin)();
  int      (*p_cart_reset_ready)();
  void     (*p_cart_reset_end)();

  // Step pacing: we call mgapi_step once per "frame" (16.67 ms) so
  // the runtime's VM/audio service advances at SNES speed. The
  // coprocessor enter() loop runs at 44100 Hz so 735 audio samples
  // = one frame; we count down to drive the step cadence.
  unsigned samples_until_step;
};

extern Mgapi mgapi;
