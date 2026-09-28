/* LSan exit-leak suppressions for the GMM test suite (Workspace-86bq).
 *
 * LSan runs at process exit and reports every allocation still live then.
 * Third-party libraries in this suite keep process-lifetime state that is
 * never freed before that check (Qt font/WebEngine singletons, GPU driver
 * state, libdbus queues, CPython static types). Each entry is one distinct
 * allocation stack observed in the failing tests, matched by substring
 * against ANY frame of the stack.
 *
 * Scope: suppressing a stack hides its whole leak cluster (the direct root
 * plus its indirect children), but a leak allocated in our own code (plain
 * new/malloc from gmm_* or from test sources) matches none of these entries
 * and still fails the run. tests/engine/lsan_selftest.c asserts exactly
 * that, so a suppression list that grows too broad turns that test red.
 *
 * Linked into every test executable by gmm_catch2_test() in
 * tests/CMakeLists.txt (no env plumbing needed - works for manual runs too).
 */
const char *__lsan_default_suppressions(void) {
  return
      /* Qt6 GUI font system (libQt6Gui + freetype + harfbuzz): face/engine
       * caches live in QFontDatabase/QFontCache singletons and are not
       * released before the exit check. Matched by function name (QFont*
       * families plus QFreetypeFace) so unrelated Qt6Gui allocations
       * (images, painted items) stay detectable. Owner: Qt. */
      "leak:QFont\n"
      "leak:QFreetypeFace\n"
      /* QtWebEngine/Chromium (libQt6WebEngineCore): the default profile and
       * WebContents scaffolding are process-lifetime objects; Qt never
       * tears them down at exit (QWebEngineProfile::defaultProfile root).
       * Owner: Qt/Chromium. */
      "leak:libQt6WebEngineCore\n"
      /* NVIDIA GL/VA-API driver state (libnvidia-glcore, libnvidia-glsi,
       * dri/nvidia_drv_video): in-process context allocations made during
       * GL/video init, never freed. Owner: NVIDIA driver. */
      "leak:nvidia\n"
      /* CUDA runtime (libcuda, reached via FFmpeg cuInit / VA-API): one-time
       * cuInit state. Owner: NVIDIA driver. */
      "leak:libcuda\n"
      /* libdbus connection message queue
       * (_dbus_message_loader_queue_messages), reached from Qt DBus during
       * WebEngine bring-up. Owner: libdbus. */
      "leak:libdbus\n"
      /* Embedded CPython (libpython3.14): immortal static type objects
       * allocated via PyType_Ready are intentionally never freed.
       * Owner: CPython. */
      "leak:_PyType_AllocNoTrack\n"
      /* Frames from a third-party .so that was dlclosed/unmapped before the
       * exit check (GL ICD / JIT style constructors): only the generic
       * '<unknown module>' frame survives symbolization. Owner: unloaded
       * third-party module. */
      "leak:unknown module\n";
}
