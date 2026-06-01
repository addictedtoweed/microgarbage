// State lives inside mgapi.dll; nothing to serialize on the bsnes
// side. The DLL's internal state (VM, audio ring, cart window, L2
// allocator) isn't part of save states — the cart guest is
// expected to handle its own save/load via SYS_AUDIO_LOAD_WAV /
// trashfs / future save APIs, not via the SNES save-state stream.
#ifdef MGAPI_CPP
void Mgapi::serialize(serializer &s) {
  // Save just the counter that paces our step() calls so a
  // serialize-deserialize round-trip resumes cleanly.
  s.integer(samples_until_step);
}
#endif
