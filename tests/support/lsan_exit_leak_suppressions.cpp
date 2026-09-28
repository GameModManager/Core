// LSan exit-leak suppressions for the WebEngine-era UI tests (Workspace-ti1p).
//
// Why this exists: LeakSanitizer scans at exit, BEFORE Qt tears down its
// process-wide caches and before Chromium (QWebEngineProfile::defaultProfile)
// releases its intentionally-live singletons. Every test whose binary touches
// the default WebEngine profile, or simply loads a font through Qt's font
// database, reports those caches as leaks and fails on exit code even though
// every assertion passed. Verified pre-existing on pristine main (2026-09-21).
//
// Delivery: __lsan_default_suppressions() is compiled INTO the test binaries
// that list this file as a source - no env var to remember, no CTest property
// to thread through Catch2's discovery plumbing (PROPERTIES values must stay
// single-valued; semicolons break Catch.cmake transport). LSan merges this
// hook with any LSAN_OPTIONS=suppressions=<file> wiring, so it coexists with
// the general exit-leak work on Workspace-86bq - reuse this file for other
// tests by adding it as an extra source, do NOT add a competing global
// suppression file.
//
// Scope discipline: patterns are module paths of third-party/system code
// only (Chromium, GPU driver, dbus, udev, freetype/fontconfig/harfbuzz, and
// Qt's font database inside libQt6Gui). Deliberate leaks in test code still
// fail - their allocation stacks do not contain these frames - verified with
// an ASan probe binary carrying identical patterns.
//
// Suppressing libQt6Gui hides leaks whose allocation happens inside Qt's
// font/image caches (QFreetypeFace, QFontEngineFT, QFontconfigDatabase);
// within these tests that is always a Qt-internal cache, never a test bug.
// NOT suppressed: libQt6Core, because every leaked QString/QList in test
// code allocates there - suppressing it would hide real bugs.
extern "C" const char *__lsan_default_suppressions() {
  return "leak:libQt6WebEngineCore\n"  // Chromium profile singletons
         "leak:libQt6Gui\n"            // Qt font/image process caches
         "leak:libfreetype\n"          // freetype face cache
         "leak:libfontconfig\n"        // fontconfig config cache
         "leak:libharfbuzz\n"          // harfbuzz shaper cache
         "leak:libnvidia\n"            // GPU driver caches (Chromium GL init)
         "leak:libdbus-1\n"            // dbus connection caches
         "leak:libudev\n";             // udev enumeration caches
}
