package cli

import (
	"encoding/json"
	"path/filepath"
	"strings"

	"github.com/arnal/corefw/internal/resolve"
)

func ids(sels []*resolve.Selected) string {
	if len(sels) == 0 {
		return "(none)"
	}
	out := make([]string, len(sels))
	for i, s := range sels {
		out[i] = s.ID()
	}
	return strings.Join(out, ", ")
}

func policyIDs(sels []*resolve.Selected) string {
	if len(sels) == 0 {
		return "(none)"
	}
	out := make([]string, len(sels))
	for i, s := range sels {
		out[i] = s.Category + "=" + s.ID()
	}
	return strings.Join(out, ", ")
}

func baseName(path string) string {
	b := filepath.Base(path)
	return strings.TrimSuffix(b, filepath.Ext(b))
}

func jsonIndent(v any) ([]byte, error) {
	return json.MarshalIndent(v, "", "  ")
}

// reorderFlagsFirst moves flag arguments (those starting with '-') ahead of
// positional ones, so `build <profile> --no-compile` parses the same as
// `build --no-compile <profile>`. Go's flag package otherwise stops at the
// first non-flag token.
func reorderFlagsFirst(args []string) []string {
	var flags, positional []string
	for i := 0; i < len(args); i++ {
		a := args[i]
		if len(a) > 0 && a[0] == '-' {
			flags = append(flags, a)
			// A flag of the form "-port /dev/x" consumes the next token unless
			// it uses "=". We can't know arity here, so rely on "=" form or
			// let flag.Parse pull the value: keep the next token with the flag
			// only when it doesn't start with '-' and isn't the sole positional.
			if !strings.Contains(a, "=") && i+1 < len(args) && !strings.HasPrefix(args[i+1], "-") {
				// Heuristic: attach following value to value-taking flags.
				if isValueFlag(a) {
					flags = append(flags, args[i+1])
					i++
				}
			}
		} else {
			positional = append(positional, a)
		}
	}
	return append(flags, positional...)
}

// isValueFlag reports whether a flag takes a value (as opposed to a bool flag).
func isValueFlag(f string) bool {
	name := strings.TrimLeft(f, "-")
	switch name {
	case "out", "firmware", "port":
		return true
	default:
		return false
	}
}
