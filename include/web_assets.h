#pragma once

// UI is embedded rather than served from a filesystem so the whole device is
// one .bin: no separate `pio run -t uploadfs` step, and OTA cannot leave the
// firmware and the UI out of step. No external references - the device is
// usually on an isolated network with no route to a CDN.
//
// Declarations only. The definitions live in web_assets.cpp because a `const`
// array at namespace scope has internal linkage, so defining these here would
// give every translation unit that included the header its own copy of all
// 17 KB. Only one file includes it today, which is luck rather than design --
// the same pattern already puts three copies of the much smaller kTiltColours
// table in the binary.
//
// The pages are split rather than whole because they are served as one
// response with the CSS and JS inlined: a page load used to cost five TCP
// connections (/, /style.css, /app.js, /api/state and a 404 for /favicon.ico)
// and five concurrent connections is exactly what fragments the heap badly
// enough to start refusing requests. Splitting keeps the shared stylesheet as a
// single copy in flash while still emitting it into both pages.

// The tilt page, emitted as: head, kStyleCss, mid, kAppJs, tail.
extern const char kIndexHead[];
extern const char kIndexMid[];
extern const char kIndexTail[];

// The firmware page, emitted as: head, kStyleCss, tail.
extern const char kOtaHead[];
extern const char kOtaTail[];

// Shared by both pages, stored once.
extern const char kStyleCss[];

extern const char kAppJs[];
