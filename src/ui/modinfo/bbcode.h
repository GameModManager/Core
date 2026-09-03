#pragma once

#include <QString>

#include <atomic>

class DescriptionBrowser;

namespace ui {

// Thin Qt adapter over the vendored libcbb single-header BBCode-to-HTML
// library (third_party/libcbb/libcbb.h). Converts QString <-> std::string
// around cbb_to_html() and frees the malloc'd C string with cbb_free().
//
// Note: libcbb rejects javascript:/data: URLs (security hardening over the
// old in-house bbcode.cpp converter) and strips CSS meta-characters from
// [color]/[size]/[font] arguments. [code] and [noparse] both imply noparse
// semantics - inner tags are not processed.
//
// bbcode_to_html is pure (string in -> string out, no globals, libcbb is
// reentrant) and is therefore safe to call from worker threads.
QString bbcode_to_html(const QString &input);

// Parse `desc` as BBCode and set the result on `browser`, off the UI thread
// when the description is long enough that the parse + QTextBrowser layout
// pass would otherwise stall the main loop. The previous render's cached
// images are cleared synchronously before dispatch (same as the sync path)
// so stale image resources cannot leak into the new document.
//
// Async dispatch uses QThreadPool::globalInstance() (the same pool
// DescriptionBrowser uses for image decode) and posts the resulting HTML
// back to the UI thread via QMetaObject::invokeMethod with a
// Qt::QueuedConnection. A QPointer<DescriptionBrowser> guard handles the
// case where the panel is destroyed mid-parse (e.g. the dialog is closed
// while a worker is running): the queued call becomes a no-op.
//
// `request_token` is a small per-panel monotonic counter the caller
// increments before every dispatch. The worker captures the value at
// dispatch time and the UI-thread callback compares against the caller's
// current value: a mismatch means the user has moved to another mod and
// the late-arriving HTML must be discarded. The pointer must outlive all
// in-flight parses for that panel; pass nullptr to disable the check (the
// last parse to complete wins, which is usually fine for a panel that
// never re-renders the same browser).
//
// Short descriptions (size() < ~1 KB) run synchronously: the thread-pool
// dispatch + queued-invoke round-trip costs more than the parse itself.
void set_bbcode_html_async(DescriptionBrowser *browser, const QString &desc,
                           std::atomic<unsigned> *request_token);

} // namespace ui
