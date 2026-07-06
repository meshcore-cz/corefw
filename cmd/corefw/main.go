// Command corefw is the host-side composition and build tool for the corefw
// MeshCore firmware platform. It parses declarative YAML profiles, resolves and
// validates components (boards, modules, policies) from built-in, local and git
// sources, generates a statically-composed PlatformIO project against the C++
// kernel, and records an exact lockfile for reproducible builds.
package main

import (
	"os"

	"github.com/arnal/corefw/internal/cli"
)

func main() {
	os.Exit(cli.Main(os.Args[1:]))
}
