// Package components embeds the built-in ("official") corefw components so a
// single self-contained binary ships with board, module and policy support out
// of the box. External components are layered on top of these at build time.
package components

import "embed"

// FS holds every built-in component's manifest, schema and defaults. The C++
// sources live under firmware/ and are referenced by path from the manifests;
// only the host-side descriptors are embedded here.
//
//go:embed all:boards all:modules all:policies
var FS embed.FS
